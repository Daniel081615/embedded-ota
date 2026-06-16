#ifndef OTA_SCHEDULER_SHARED_H
#define OTA_SCHEDULER_SHARED_H

#include <stdint.h>
#include "fw_info.h"

/*
 * 共用 OTA 排程器介面（Single Source of Truth）
 * ------------------------------------------------------------------
 * Store-and-Trigger 接收狀態機，以 OtaRole_t 參數化，一份源碼服務三種角色：
 *   OTA_ROLE_CENTER：Gateway，接收 HOST 韌體，TriggerProxy 委派 OtaCenterProxy/Relay
 *   OTA_ROLE_METER ：Concentrator，接收 CENTER 韌體，TriggerProxy 委派 OtaMeterProxy
 *   OTA_ROLE_METER ：RoomNode（leaf），同角色接收 METER 韌體，TriggerProxy 進 Bootloader
 *
 * 轉發實作全在被注入的 TriggerProxy 回呼裡；排程器只負責 Store 階段
 * （接收 + 逐包寫 Flash + 本地 CRC 驗證）。實作見 ota_scheduler.c。
 *
 * 工具鏈：純 C，需同時通過 Keil armclang 與 Atmel Studio arm-gcc。
 */

/* ── 1. OTA 通訊指令碼 (與 AO_HostProcess.h MSG_HOST_CMD_OTA_* 數值對齊) ── */
typedef enum {
    OTA_CMD_ENTER_REQ         = 0x20, /* 上游→下游: 請求進入 OTA 模式           */
    OTA_CMD_STATUS_RSP        = 0x21, /* 下游→上游: CRC 驗證通過，等候 TRIGGER  */
    OTA_CMD_UPDATE_LOCAL_REQ  = 0x22, /* 上游→自身: 更新本節點 (CENTER 自更新)   */
    OTA_CMD_UPDATE_CHILD_REQ  = 0x23, /* 上游→自身: 準備接收子節點韌體           */
    OTA_CMD_STORE_CHILD_REQ   = 0x24, /* 上游→自身: 子節點韌體資料 chunk         */
    OTA_CMD_TRIGGER_CHILD_REQ = 0x25, /* 上游→自身: 開始轉發給子節點             */
    OTA_CMD_APP_READY         = 0x26, /* 下游→上游: 子節點新 App 啟動完成通知    */
    OTA_CMD_ENTER_RSP         = 0x40, /* 下游→上游: 收到 ENTER_REQ / chunk ACK  */
    OTA_CMD_ERROR_RSP         = 0xFF  /* 任意方向:  錯誤通知                     */
} OtaCommand_t;

/* ── 2. 角色定義 ── */
typedef enum {
    OTA_ROLE_CENTER = 0, /* 上游是 HOST，下游是多台 METER */
    OTA_ROLE_METER  = 1  /* 上游是 CENTER，下游是 1 台 READER（RoomNode 以此角色接收） */
} OtaRole_t;

/* ── 3. 逾時 / 重試常數 ── */
#define OTA_RECV_TIMEOUT_MS      30000u /* 逐包閒置逾時（兩包之間最多等 30 s）  */
#define OTA_FWD_INIT_TIMEOUT_MS   3000u /* 等待下游回 ENTER_RSP 握手逾時（3 s）*/
#define OTA_CHUNK_TIMEOUT_MS       300u /* 等待下游回每包 ACK 的逾時（300 ms） */
#define OTA_VERIFY_TIMEOUT_MS    10000u /* 等待下游回 STATUS_RSP 的逾時（10 s）*/
#define OTA_MAX_RETRY               3u  /* 超過此次數視為節點失敗              */
#define OTA_CHUNK_SIZE             92u  /* 每包 payload 大小（100-byte frame） */

/* ── 4. 通用狀態機 ── */
typedef enum {
    OTA_STATE_IDLE = 0,

    /* 接收期 (Store Phase)：接收上游韌體，寫入本地 Bank */
    OTA_STATE_RECV_INIT,     /* 收到 UPDATE_CHILD_REQ，開始擦除本地 Bank  */
    OTA_STATE_RECV_ERASING,  /* 擦除中（BSP 同步阻塞，通常只停留一個 tick）*/
    OTA_STATE_RECV_DATA,     /* 逐包接收上游資料，寫入 Flash              */
    OTA_STATE_VERIFY_LOCAL,  /* 接收完成，驗證本地 CRC32                  */

    /* 轉發期 (Forward Phase)：將本地 Bank 逐包送給下游子節點 */
    OTA_STATE_FORWARD_INIT,     /* 向下游發 ENTER_REQ，等握手             */
    OTA_STATE_FORWARD_WAIT_ACK, /* 等待下游 ACK；逾時重傳                 */
    OTA_STATE_FORWARD_DATA,     /* 讀一 chunk，發送給下游                 */
    OTA_STATE_FORWARD_VERIFY,   /* 全部發完，等下游回 STATUS_RSP（CRC ok）*/
    OTA_STATE_FORWARD_NEXT,     /* (CENTER 適用) 切換到下一台子節點       */

    OTA_STATE_ERROR             /* 不可恢復錯誤，等候 IDLE 復原           */
} OtaState_t;

/* ── 5. 系統服務介面 ── */
typedef struct {
    /* 單調遞增毫秒計數；32-bit 溢位以無號差值運算自然處理 */
    uint32_t (*GetTickMs)(void);
} IOtaSys_t;

/* ── 6. 網路通訊介面 ── */
typedef struct {
    /* 往上游發送（METER→CENTER 或 CENTER→HOST）*/
    int32_t (*SendUpstream)(uint8_t cmd, const uint8_t *payload, uint16_t len);

    /* 往下游發送（CENTER→METER 或 METER→READER）
     * node_id：CENTER 指定目標 METER ID；METER→READER 時可傳 0 */
    int32_t (*SendDownstream)(uint8_t node_id, uint8_t cmd,
                              const uint8_t *payload, uint16_t len);

    /* 觸發 Proxy 模組接管後續轉發
     * CENTER 端：OtaCenterProxy/Relay；METER 端：OtaMeterProxy；
     * RoomNode（leaf）：進 Bootloader（target_node_id 忽略） */
    void    (*TriggerProxy)(uint8_t bank_index, uint32_t payload_size,
                            uint32_t transport_crc32, uint32_t fw_image_size,
                            uint32_t version, uint8_t target_node_id);
} IOtaNet_t;

/* ── 7. 排程器上下文 ── */
typedef struct {
    OtaRole_t  role;             /* 當前設備角色 */
    OtaState_t state;            /* 當前狀態     */
    uint8_t    target_bank;      /* 本地暫存 Bank（0 = Bank1，1 = Bank2） */
    uint8_t    target_node_id;   /* 下游目標節點 ID（CENTER 填 METER ID，METER 填 Reader ID）*/

    uint32_t   fw_total_size;    /* 接收 payload 總 byte 數（= payload_size）         */
    uint32_t   fw_expected_crc;  /* 預期 transport CRC32                              */
    uint32_t   current_offset;   /* 目前已處理位移（接收 / 轉發共用）                  */
    uint32_t   fw_image_size;    /* 原始韌體大小（TriggerProxy 轉發用）               */
    uint32_t   version;          /* 版本號（TriggerProxy 轉發用）                     */

    uint8_t    retry_count;       /* 重試計數（超過 OTA_MAX_RETRY 進入 ERROR）*/
    uint32_t   send_timestamp_ms; /* 最近一次發包時刻，用於 per-chunk 逾時判斷 */
    uint32_t   phase_deadline_ms; /* 階段全域截止時刻（接收期 / 驗證期）        */
} OtaSchedulerCtx_t;

/* ── 8. 公開 API ── */
void OtaScheduler_Init(OtaRole_t role,
                       const IOtaNet_t *net_driver,
                       const IOtaSys_t *sys_driver);
void OtaScheduler_Task(void);
void OtaScheduler_OnReceivePacket(uint8_t cmd,
                                  const uint8_t *payload,
                                  uint16_t len);
_Bool OtaScheduler_IsActive(void); /* IDLE / ERROR 以外的所有狀態均視為 active */

#endif /* OTA_SCHEDULER_SHARED_H */
