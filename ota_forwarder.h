#ifndef OTA_FORWARDER_SHARED_H
#define OTA_FORWARDER_SHARED_H

#include <stdint.h>
#include "ota_scheduler.h"   /* IOtaSys_t, OtaCommand_t (OTA_CMD_*), OTA_CHUNK_SIZE */

/*
 * 共用 OTA forwarder 核心（Single Source of Truth）— Phase 5
 * ------------------------------------------------------------------
 * 「上游節點逐台燒錄下游子節點」的非同步狀態機，把先前各自一份的
 * ota_center_proxy.c / ota_meter_proxy.c（兩支 BL 代理人，狀態機逐行相同、
 * 只差傳輸層）收斂成單一核心。差異全部外包成兩個正交 vtable：
 *
 *   IOtaFwdTx_t      傳輸層原語（怎麼把 100-byte frame 送出 / flush）—— 每節點一份
 *   IOtaFwdProfile_t 轉發語意（要不要 ENTER 握手、UPDATE 20/21-byte、完成/失敗動作）
 *
 * 目前只有 PROXY_PROFILE（BL 握手代理人）。未來收斂 center_relay（App relay）時，
 * 只需新增 RELAY_PROFILE + 把 center_relay.c 改成 wrapper，**不動本核心**：
 * FSM 已含 relay 所需的 handshake-skip 分支與 OTA_FWD_TRIGGERING 尾段狀態。
 *
 * actor 形狀：context 由 caller 持有並傳入，無單例 file-static，未來上 RTOS
 * 每個 forwarder = 一個 task-private context。
 *
 * 工具鏈：純 C，需同時通過 Keil armclang 與 Atmel Studio arm-gcc。
 */

/* 100-byte 自訂 UART frame：[0x55][id][cmd][payload..][checksum][0x0A] */
#define OTA_FWD_FRAME_BUF   100u

/* ── 逾時 / 重試常數（與兩支 proxy 既有值一致）── */
#ifndef OTA_FWD_INIT_TIMEOUT_MS          /* ota_scheduler.h 已定義同名同值 3000 */
#define OTA_FWD_INIT_TIMEOUT_MS    3000u /* 等下游回 ENTER_RSP 握手           */
#endif
#define OTA_FWD_UPDATE_TIMEOUT_MS  5000u /* 等下游回 UPDATE_META 的 STATUS_RSP */
#define OTA_FWD_CHUNK_TIMEOUT_MS    300u /* 等下游回每包 chunk ACK            */
#define OTA_FWD_APP_READY_TIMEOUT_MS 2000u /* 等子節點新 App 送 APP_READY     */
#define OTA_FWD_SETTLE_MS             50u /* 單台完成後 UART 沉澱             */
#define OTA_FWD_MAX_RETRY              3u

/* ── 狀態機（superset；數值 0..8 與舊 OtaCenterProxyState_t/OtaProxyState_t 完全對齊，
 *    故 wrapper 可直接值保留 cast；TRIGGERING=9 為 relay 專用尾段，PROXY_PROFILE 永不進入）── */
typedef enum {
    OTA_FWD_IDLE = 0,
    OTA_FWD_TRIGGERED,        /* 下一 tick 對當前子節點起步（送 ENTER 或直接 UPDATE）*/
    OTA_FWD_WAIT_ENTER_ACK,   /* 等下游 BL ENTER_RSP (0x40)（僅握手 profile）        */
    OTA_FWD_WAIT_UPDATE_ACK,  /* 已送 UPDATE_META，等 STATUS_RSP (0x21)             */
    OTA_FWD_FLASHING,         /* 逐包傳輸 STORE [offset+data]                       */
    OTA_FWD_UART_SETTLE,      /* 單台完成後清 UART，等 50ms HW 穩定（握手 profile）  */
    OTA_FWD_WAIT_APP_READY,   /* 等子節點新 App 送 APP_READY（握手 profile）         */
    OTA_FWD_DONE,             /* 全部子節點處理完（部分可能 fail）                   */
    OTA_FWD_ERROR,            /* 不可恢復錯誤（Flash 讀取失敗等）                    */
    OTA_FWD_TRIGGERING        /* 送 TRIGGER_CHILD_REQ 後即切下一台（relay 專用尾段） */
} OtaFwdState_t;

/* ── 進度快照 ── */
typedef struct {
    uint8_t cur_child;  /* 當前處理中的子節點 ID（1-based）*/
    uint8_t total;      /* 子節點總數                      */
    uint8_t done_cnt;   /* 成功燒錄台數                    */
    uint8_t fail_cnt;   /* 失敗（含逾時放棄）台數          */
} OtaFwdProgress_t;

/* ── 傳輸層 vtable（每節點一份 adapter）── */
typedef struct {
    /* 建框 + 入 TxQ。buf[2]=cmd、buf[3..] 已填；adapter 負責 [0]=0x55 / [1]=child_id /
     * checksum / [last]=0x0A 並送出。回傳 1=成功，0=TxQ 滿（本 tick 略過，下 tick 重試）。*/
    _Bool (*SendPacket)(uint8_t *frame_buf, uint16_t frame_len, uint8_t child_id);
    void  (*Settle)(void);    /* FlushAndSettle 用（軟清；DM=UART_Reset、GW=HardFlush）*/
    void  (*HardFlush)(void); /* 整 session DONE 用（硬清）                            */
} IOtaFwdTx_t;

struct OtaFwdCtx;  /* forward */

/* ── 轉發語意 vtable（profile）── */
typedef struct {
    _Bool   use_enter_handshake;                                  /* proxy=1, relay=0 */
    /* 把 UPDATE_META payload 寫入 pld，回傳長度（proxy=20，relay=21 含 node_id 前綴）。
     * 20/21-byte 配對只活在此函式，共用 FSM 永不碰。*/
    uint8_t (*EncodeUpdate)(uint8_t *pld, const struct OtaFwdCtx *c);
    void    (*OnStreamComplete)(struct OtaFwdCtx *c); /* 末包後：proxy=done++ &Settle / relay=進 TRIGGERING */
    void    (*OnChildFail)(struct OtaFwdCtx *c);      /* 失敗：proxy=fail++ &Settle / relay=下一台          */
} IOtaFwdProfile_t;

/* ── context（caller 持有，actor-private）── */
typedef struct OtaFwdCtx {
    const IOtaSys_t        *sys;        /* GetTickMs */
    const IOtaFwdTx_t      *tx;
    const IOtaFwdProfile_t *profile;
    uint8_t  child_max;                /* 子節點總數，Init 後不變 */
    uint16_t frame_len;                /* = MAX_*_TOKEN_LENGTH (100) */

    OtaFwdState_t state;
    uint8_t  bank_index;
    uint8_t  cur_child_idx;            /* 0-based；child_id = idx+1 */
    uint8_t  session_end;              /* 本次 session 終止邊界（exclusive）*/
    uint8_t  done_cnt;
    uint8_t  fail_cnt;
    uint8_t  retry_cnt;
    uint32_t payload_size;
    uint32_t transport_crc32;
    uint32_t fw_image_size;
    uint32_t version;
    uint32_t cur_offset;              /* 當前子節點已確認的 byte 偏移 */
    uint32_t deadline_ms;

    uint8_t  tx_buf[OTA_FWD_FRAME_BUF];
    uint8_t  chunk_buf[OTA_CHUNK_SIZE + 3u];  /* +3：最後一包 4-byte 對齊讀取 */
} OtaFwdCtx_t;

/* ── 內建 profile：BL 握手代理人（center_proxy / meter_proxy 共用）── */
extern const IOtaFwdProfile_t OTA_FWD_PROXY_PROFILE;

/* ── 公開 API ── */
void OtaForwarder_Init(OtaFwdCtx_t *c, const IOtaSys_t *sys, const IOtaFwdTx_t *tx,
                       const IOtaFwdProfile_t *profile, uint8_t child_max, uint16_t frame_len);

/* TriggerProxy 回呼。target_id：0 = 廣播全部，N = 只燒第 N 台（1-based）*/
void OtaForwarder_Trigger(OtaFwdCtx_t *c, uint8_t bank_index, uint32_t payload_size,
                          uint32_t transport_crc32, uint32_t fw_image_size,
                          uint32_t version, uint8_t target_id);

void OtaForwarder_Task(OtaFwdCtx_t *c);

/* 收到子節點 BL/App 封包時呼叫。child_id=token[1]（1-based），ota_cmd=token[2] */
void OtaForwarder_OnChildAck(OtaFwdCtx_t *c, uint8_t child_id, uint8_t ota_cmd);

_Bool            OtaForwarder_IsBusy(const OtaFwdCtx_t *c);
OtaFwdState_t    OtaForwarder_GetState(const OtaFwdCtx_t *c);
OtaFwdProgress_t OtaForwarder_GetProgress(const OtaFwdCtx_t *c);

#endif /* OTA_FORWARDER_SHARED_H */
