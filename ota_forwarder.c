/*
 * Shared/ota/ota_forwarder.c
 *
 * 共用 OTA forwarder 核心 — 詳見 ota_forwarder.h。
 *
 * PROXY_PROFILE（BL 握手代理人）每台子節點流程：
 *
 *   OTA_FWD_TRIGGERED
 *     Task(): 送 ENTER_REQ (0x20) → WAIT_ENTER_ACK
 *
 *   OTA_FWD_WAIT_ENTER_ACK
 *     OnAck(ENTER_RSP=0x40): 送 UPDATE_META → WAIT_UPDATE_ACK
 *     Task() 逾時: 重試 ENTER_REQ，超過 MAX_RETRY → OnChildFail
 *
 *   OTA_FWD_WAIT_UPDATE_ACK
 *     OnAck(STATUS_RSP=0x21): cur_offset=0 → 送第一包 → FLASHING
 *     Task() 逾時: 重試 UPDATE_META，超過 MAX_RETRY → OnChildFail
 *
 *   OTA_FWD_FLASHING
 *     OnAck(STATUS_RSP=0x21): 推進 offset → 下一包，或末包 → OnStreamComplete
 *     OnAck(ERROR_RSP=0xFF): OnChildFail
 *     Task() 逾時: 重試上一包，超過 MAX_RETRY → OnChildFail
 *
 *   OTA_FWD_UART_SETTLE (50ms) → WAIT_APP_READY
 *   OTA_FWD_WAIT_APP_READY
 *     OnAck(APP_READY=0x26) 或 Task() 逾時容忍 → AdvanceNext
 *
 * RELAY_PROFILE（App relay，尚未實作）會走 handshake-skip 與 OTA_FWD_TRIGGERING
 * 尾段；本核心已備妥這兩條路徑，屆時只需新增 RELAY_PROFILE。
 */

#include "ota_forwarder.h"
#include "ota_protocol.h"
#include "flash_service.h"
#include <string.h>

/* ════════════════════════════════════════════════════════════════
 *  私有工具
 * ════════════════════════════════════════════════════════════════ */

static uint32_t GetTick(const OtaFwdCtx_t *c)
{
    return (c->sys && c->sys->GetTickMs) ? c->sys->GetTickMs() : 0u;
}

static uint8_t ChildIdOf(const OtaFwdCtx_t *c)
{
    return (uint8_t)(c->cur_child_idx + 1u);
}

/*
 * 建 100-byte frame（buf[2]=cmd、buf[3..]=payload）並交給 transport 送出。
 * 回傳 1=已入 TxQ，0=TxQ 滿（下個 tick 重試）。
 */
static _Bool SendCmd(OtaFwdCtx_t *c, uint8_t cmd, const uint8_t *payload, uint8_t payload_len)
{
    if (payload_len > (uint8_t)(c->frame_len - 5u))
        return 0;

    memset(c->tx_buf, 0u, c->frame_len);
    c->tx_buf[2] = cmd;
    if (payload != NULL && payload_len > 0u)
        memcpy(&c->tx_buf[3], payload, payload_len);

    return c->tx->SendPacket(c->tx_buf, c->frame_len, ChildIdOf(c));
}

static _Bool SendEnterReq(OtaFwdCtx_t *c)
{
    return SendCmd(c, (uint8_t)OTA_CMD_ENTER_REQ, NULL, 0u);
}

/* UPDATE_META：payload 編碼交給 profile（20 或 21-byte）*/
static _Bool SendUpdateMeta(OtaFwdCtx_t *c)
{
    uint8_t pld[OTA_PROTO_UPDATE_META_LEN + 1u];  /* +1：relay 的 node_id 前綴 */
    uint8_t len = c->profile->EncodeUpdate(pld, c);
    return SendCmd(c, (uint8_t)OTA_CMD_UPDATE_CHILD_REQ, pld, len);
}

/*
 * 從 Flash 讀 cur_offset 的 chunk，以 [offset(4B)][data(88B)] 送出。
 * chunk_buf[0..3]=offset LE、chunk_buf[4..]=firmware data。
 */
static _Bool SendNextChunk(OtaFwdCtx_t *c)
{
    const uint16_t DATA_SIZE = (uint16_t)(OTA_CHUNK_SIZE - 4u);  /* 88 bytes */
    uint32_t remaining = c->payload_size - c->cur_offset;
    uint16_t data_size = (remaining >= (uint32_t)DATA_SIZE) ? DATA_SIZE : (uint16_t)remaining;
    uint16_t read_size = (uint16_t)((data_size + 3u) & ~3u);

    OtaProto_EncodeChunkOffset(c->chunk_buf, c->cur_offset);

    if (FlashService_ReadFirmware(c->bank_index, c->cur_offset,
                                  c->chunk_buf + 4u, read_size) != 0) {
        c->state = OTA_FWD_ERROR;
        return 0;
    }

    return SendCmd(c, (uint8_t)OTA_CMD_STORE_CHILD_REQ, c->chunk_buf, (uint8_t)(4u + data_size));
}

/* 送 TRIGGER_CHILD_REQ（relay 尾段用；PROXY_PROFILE 不會走到）*/
static _Bool SendTrigger(OtaFwdCtx_t *c)
{
    return SendCmd(c, (uint8_t)OTA_CMD_TRIGGER_CHILD_REQ, NULL, 0u);
}

/* 清 UART 並進入 50ms 沉澱期，到期後再進 WAIT_APP_READY */
static void FlushAndSettle(OtaFwdCtx_t *c)
{
    c->tx->Settle();
    c->deadline_ms = GetTick(c) + OTA_FWD_SETTLE_MS;
    c->state       = OTA_FWD_UART_SETTLE;
}

/* 推進到下一台子節點，或宣告全部完成 */
static void AdvanceNext(OtaFwdCtx_t *c)
{
    c->cur_child_idx++;
    if (c->cur_child_idx >= c->session_end) {
        c->state = OTA_FWD_DONE;
        c->tx->HardFlush();
        return;
    }
    c->cur_offset = 0u;
    c->retry_cnt  = 0u;
    c->state      = OTA_FWD_TRIGGERED;
}

/* ════════════════════════════════════════════════════════════════
 *  PROXY_PROFILE（BL 握手代理人）profile hooks
 * ════════════════════════════════════════════════════════════════ */

static uint8_t Proxy_EncodeUpdate(uint8_t *pld, const OtaFwdCtx_t *c)
{
    OtaProto_EncodeUpdateMeta(pld, c->payload_size, c->fw_image_size,
                              c->transport_crc32, c->version);
    return OTA_PROTO_UPDATE_META_LEN;   /* 20 */
}

static void Proxy_OnStreamComplete(OtaFwdCtx_t *c)
{
    c->done_cnt++;
    FlushAndSettle(c);
}

static void Proxy_OnChildFail(OtaFwdCtx_t *c)
{
    c->fail_cnt++;
    FlushAndSettle(c);
}

const IOtaFwdProfile_t OTA_FWD_PROXY_PROFILE = {
    1,                       /* use_enter_handshake */
    Proxy_EncodeUpdate,
    Proxy_OnStreamComplete,
    Proxy_OnChildFail
};

/* ════════════════════════════════════════════════════════════════
 *  RELAY_PROFILE（CENTER→METER App 中繼人）profile hooks
 *
 *  與 PROXY 的差異全在此三 hook + use_enter_handshake=0：
 *    - 無 BL 握手：TRIGGERED 直接送 UPDATE（FSM 的 handshake-skip 分支）
 *    - UPDATE 為 21-byte（含 node_id=0 前綴；20/21 配對只在此函式）
 *    - 末包後不等 APP_READY，改進 OTA_FWD_TRIGGERING 送 TRIGGER 後即切下一台
 *    - 失敗直接切下一台（不計 fail、不 settle）
 *  本區塊純附加，未更動 PROXY 路徑（已實機驗證）。
 * ════════════════════════════════════════════════════════════════ */

static uint8_t Relay_EncodeUpdate(uint8_t *pld, const OtaFwdCtx_t *c)
{
    pld[0] = 0u;   /* node_id = 0：廣播該 METER 下所有子節點 */
    OtaProto_EncodeUpdateMeta(&pld[1], c->payload_size, c->fw_image_size,
                              c->transport_crc32, c->version);
    return (uint8_t)(1u + OTA_PROTO_UPDATE_META_LEN);   /* 21 */
}

static void Relay_OnStreamComplete(OtaFwdCtx_t *c)
{
    c->state = OTA_FWD_TRIGGERING;   /* 下個 tick 送 TRIGGER_CHILD_REQ */
}

static void Relay_OnChildFail(OtaFwdCtx_t *c)
{
    AdvanceNext(c);
}

const IOtaFwdProfile_t OTA_FWD_RELAY_PROFILE = {
    0,                       /* use_enter_handshake = 0（App relay，無 BL 握手）*/
    Relay_EncodeUpdate,
    Relay_OnStreamComplete,
    Relay_OnChildFail
};

/* ════════════════════════════════════════════════════════════════
 *  公開 API
 * ════════════════════════════════════════════════════════════════ */

void OtaForwarder_Init(OtaFwdCtx_t *c, const IOtaSys_t *sys, const IOtaFwdTx_t *tx,
                       const IOtaFwdProfile_t *profile, uint8_t child_max, uint16_t frame_len)
{
    memset(c, 0, sizeof(*c));
    c->sys       = sys;
    c->tx        = tx;
    c->profile   = profile;
    c->child_max = child_max;
    c->frame_len = frame_len;
    c->state     = OTA_FWD_IDLE;
}

void OtaForwarder_Trigger(OtaFwdCtx_t *c, uint8_t bank_index, uint32_t payload_size,
                          uint32_t transport_crc32, uint32_t fw_image_size,
                          uint32_t version, uint8_t target_id)
{
    if (OtaForwarder_IsBusy(c)) return;   /* 傳輸中才拒絕；DONE/ERROR 允許重觸發 */
    if (payload_size == 0u) return;

    c->bank_index      = bank_index;
    c->payload_size    = payload_size;
    c->transport_crc32 = transport_crc32;
    c->fw_image_size   = fw_image_size;
    c->version         = version;
    c->done_cnt        = 0u;
    c->fail_cnt        = 0u;
    c->cur_offset      = 0u;
    c->retry_cnt       = 0u;

    /* target_id = 0：廣播全部；N（1-based）：只燒第 N 台 */
    if (target_id == 0u || target_id > c->child_max) {
        c->cur_child_idx = 0u;
        c->session_end   = c->child_max;
    } else {
        c->cur_child_idx = (uint8_t)(target_id - 1u);
        c->session_end   = target_id;
    }

    c->state = OTA_FWD_TRIGGERED;
}

void OtaForwarder_Task(OtaFwdCtx_t *c)
{
    uint32_t now = GetTick(c);

    switch (c->state) {

    case OTA_FWD_IDLE:
    case OTA_FWD_DONE:
    case OTA_FWD_ERROR:
        break;

    case OTA_FWD_TRIGGERED:
        if (c->profile->use_enter_handshake) {
            if (!SendEnterReq(c)) break;     /* TxQ 暫滿，下 tick 重試 */
            c->deadline_ms = now + OTA_FWD_INIT_TIMEOUT_MS;
            c->retry_cnt   = 0u;
            c->state       = OTA_FWD_WAIT_ENTER_ACK;
        } else {
            if (!SendUpdateMeta(c)) break;   /* relay：略過握手，直接 UPDATE */
            c->deadline_ms = now + OTA_FWD_UPDATE_TIMEOUT_MS;
            c->retry_cnt   = 0u;
            c->state       = OTA_FWD_WAIT_UPDATE_ACK;
        }
        break;

    case OTA_FWD_WAIT_ENTER_ACK:
        if ((int32_t)(now - c->deadline_ms) < 0) break;
        c->retry_cnt++;
        if (c->retry_cnt >= OTA_FWD_MAX_RETRY) { c->profile->OnChildFail(c); break; }
        if (SendEnterReq(c))
            c->deadline_ms = now + OTA_FWD_INIT_TIMEOUT_MS;
        break;

    case OTA_FWD_WAIT_UPDATE_ACK:
        if ((int32_t)(now - c->deadline_ms) < 0) break;
        c->retry_cnt++;
        if (c->retry_cnt >= OTA_FWD_MAX_RETRY) { c->profile->OnChildFail(c); break; }
        if (SendUpdateMeta(c))
            c->deadline_ms = now + OTA_FWD_UPDATE_TIMEOUT_MS;
        break;

    case OTA_FWD_FLASHING:
        if ((int32_t)(now - c->deadline_ms) < 0) break;
        c->retry_cnt++;
        if (c->retry_cnt >= OTA_FWD_MAX_RETRY) { c->profile->OnChildFail(c); break; }
        if (SendNextChunk(c))
            c->deadline_ms = now + OTA_FWD_CHUNK_TIMEOUT_MS;
        break;

    case OTA_FWD_UART_SETTLE:
        if ((int32_t)(now - c->deadline_ms) < 0) break;
        c->deadline_ms = now + OTA_FWD_APP_READY_TIMEOUT_MS;
        c->state       = OTA_FWD_WAIT_APP_READY;
        break;

    case OTA_FWD_WAIT_APP_READY:
        if ((int32_t)(now - c->deadline_ms) < 0) break;
        AdvanceNext(c);   /* 逾時容忍：子節點未送 APP_READY，仍繼續下一台 */
        break;

    case OTA_FWD_TRIGGERING:           /* relay 尾段（PROXY_PROFILE 不會進入）*/
        if (SendTrigger(c))
            AdvanceNext(c);            /* 不等 ACK */
        break;

    default:
        break;
    }
}

void OtaForwarder_OnChildAck(OtaFwdCtx_t *c, uint8_t child_id, uint8_t ota_cmd)
{
    if (child_id != ChildIdOf(c)) return;

    if (c->state != OTA_FWD_WAIT_ENTER_ACK  &&
        c->state != OTA_FWD_WAIT_UPDATE_ACK &&
        c->state != OTA_FWD_FLASHING        &&
        c->state != OTA_FWD_WAIT_APP_READY) return;

    uint32_t now = GetTick(c);

    switch (ota_cmd) {

    case OTA_CMD_ENTER_RSP:   /* 0x40 — 握手完成，送 UPDATE_META */
        if (c->state != OTA_FWD_WAIT_ENTER_ACK) break;
        c->retry_cnt = 0u;
        if (SendUpdateMeta(c)) {
            c->deadline_ms = now + OTA_FWD_UPDATE_TIMEOUT_MS;
            c->retry_cnt   = 0u;
            c->state       = OTA_FWD_WAIT_UPDATE_ACK;
        }
        break;

    case OTA_CMD_STATUS_RSP:  /* 0x21 — UPDATE_META ACK 或 STORE chunk ACK */
        c->retry_cnt = 0u;

        if (c->state == OTA_FWD_WAIT_UPDATE_ACK) {
            c->cur_offset = 0u;
            if (SendNextChunk(c)) {
                c->deadline_ms = now + OTA_FWD_CHUNK_TIMEOUT_MS;
                c->state       = OTA_FWD_FLASHING;
            }
        } else if (c->state == OTA_FWD_FLASHING) {
            const uint16_t DATA_SIZE = (uint16_t)(OTA_CHUNK_SIZE - 4u);
            uint32_t remaining = c->payload_size - c->cur_offset;
            uint32_t advance   = (remaining >= (uint32_t)DATA_SIZE)
                                 ? (uint32_t)DATA_SIZE : remaining;
            c->cur_offset += advance;

            if (c->cur_offset < c->payload_size) {
                if (SendNextChunk(c))
                    c->deadline_ms = now + OTA_FWD_CHUNK_TIMEOUT_MS;
            } else {
                c->profile->OnStreamComplete(c);   /* 末包：proxy=done++&Settle / relay=TRIGGERING */
            }
        }
        break;

    case OTA_CMD_APP_READY:   /* 0x26 — 子節點新 App 啟動完成 */
        if (c->state != OTA_FWD_WAIT_APP_READY) break;
        AdvanceNext(c);
        break;

    case OTA_CMD_ERROR_RSP:   /* 0xFF — 下游回報失敗 */
        c->profile->OnChildFail(c);
        break;

    default:
        break;
    }
}

_Bool OtaForwarder_IsBusy(const OtaFwdCtx_t *c)
{
    return (c->state == OTA_FWD_TRIGGERED      ||
            c->state == OTA_FWD_WAIT_ENTER_ACK ||
            c->state == OTA_FWD_WAIT_UPDATE_ACK||
            c->state == OTA_FWD_FLASHING       ||
            c->state == OTA_FWD_UART_SETTLE    ||
            c->state == OTA_FWD_WAIT_APP_READY ||
            c->state == OTA_FWD_TRIGGERING);
}

OtaFwdState_t OtaForwarder_GetState(const OtaFwdCtx_t *c)
{
    return c->state;
}

OtaFwdProgress_t OtaForwarder_GetProgress(const OtaFwdCtx_t *c)
{
    OtaFwdProgress_t p;
    p.cur_child = ChildIdOf(c);
    p.total     = c->child_max;
    p.done_cnt  = c->done_cnt;
    p.fail_cnt  = c->fail_cnt;
    return p;
}
