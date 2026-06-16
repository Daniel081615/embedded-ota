/* ota_scheduler.c
 *
 * 通用 OTA 排程器（Store-and-Trigger 架構）— Single Source of Truth。
 * 同一份源碼以 OtaRole_t 服務三種角色：
 *   OTA_ROLE_CENTER：接收 HOST 推送的韌體，驗證後透過 TriggerProxy 委派 OtaCenterProxy/Relay
 *   OTA_ROLE_METER ：接收 CENTER 推送的韌體，驗證後透過 TriggerProxy 委派 OtaMeterProxy
 *   OTA_ROLE_METER ：RoomNode（leaf）同角色接收 METER 韌體，TriggerProxy 進 Bootloader
 *
 * 轉發邏輯完全由 TriggerProxy 回呼（IOtaNet_t.TriggerProxy）的被注入函式負責；
 * 排程器本身只負責 Store 階段（接收 + 本地 CRC 驗證），保持職責單一。
 *
 * 呼叫端責任：
 *   - 每個主迴圈 tick 呼叫 OtaScheduler_Task()
 *   - 收到上游 OTA 封包時呼叫 OtaScheduler_OnReceivePacket()
 *     （dispatcher 傳入的 len 必須 == 實際 frame payload；STORE 上限 OTA_CHUNK_SIZE）
 */

#include "ota_scheduler.h"
#include "flash_service.h"
#include <stddef.h>

/* ── 模組靜態變數 ── */
static OtaSchedulerCtx_t s_ctx;
static const IOtaNet_t  *s_net = NULL;
static const IOtaSys_t  *s_sys = NULL;

/* ── 私有工具函式 ── */

static uint32_t GetTick(void)
{
    return (s_sys && s_sys->GetTickMs) ? s_sys->GetTickMs() : 0u;
}

static void SendUp(uint8_t cmd)
{
    if (s_net && s_net->SendUpstream)
        s_net->SendUpstream(cmd, NULL, 0);
}

static void EnterError(void)
{
    s_ctx.state = OTA_STATE_ERROR;
}

/* ── 公開 API ── */

void OtaScheduler_Init(OtaRole_t           role,
                       const IOtaNet_t    *net_driver,
                       const IOtaSys_t    *sys_driver)
{
    s_ctx.role           = role;
    s_ctx.state          = OTA_STATE_IDLE;
    s_ctx.target_bank    = 1u; /* 預設 Non-Active Bank */
    s_ctx.target_node_id = 0u;
    s_ctx.current_offset = 0u;
    s_ctx.retry_count    = 0u;
    s_net = net_driver;
    s_sys = sys_driver;
}

/* ────────────────────────────────────────────────────────
 *  OtaScheduler_Task
 *  每個主迴圈 tick 呼叫一次。
 *  Task 本身只驅動「接收期主動發包」與「逾時偵測」；
 *  狀態推進的另一半由 OnReceivePacket() 完成。
 * ──────────────────────────────────────────────────────── */
void OtaScheduler_Task(void)
{
    uint32_t now = GetTick();

    switch (s_ctx.state) {

    case OTA_STATE_IDLE:
        break;

    /* ── 接收期：擦除本地 Bank ── */
    case OTA_STATE_RECV_INIT:
        s_ctx.state = OTA_STATE_RECV_ERASING;
        if (FlashService_EraseBank(s_ctx.target_bank) == 0) {
            s_ctx.current_offset    = 0u;
            s_ctx.phase_deadline_ms = now + OTA_RECV_TIMEOUT_MS;
            s_ctx.state = OTA_STATE_RECV_DATA;
            /* 標記 bank 用途（Data Flash，不受 APROM 擦除影響）*/
            {
                FW_Info_t fi;
                if (FlashService_ReadFWInfo(&fi) == 0) {
                    FW_BankUsage_t u = (s_ctx.role == OTA_ROLE_CENTER)
                                       ? BANK_USAGE_RELAY_METER
                                       : BANK_USAGE_RELAY_READER;
                    if (s_ctx.target_bank == 0u) fi.bank0_usage = u;
                    else                          fi.bank1_usage = u;
                    (void)FlashService_UpdateFWInfo(&fi);
                }
            }
            /* 通知上游：Bank 已擦除完成，請開始推送資料 */
            {
                uint8_t zero = 0x00;
                if (s_net && s_net->SendUpstream)
                    s_net->SendUpstream(OTA_CMD_STATUS_RSP, &zero, 1u);
            }
        } else {
            EnterError();
        }
        break;

    case OTA_STATE_RECV_ERASING:
        /* BSP ErasePage 為同步阻塞；正常情況不會停留在此態超過一個 tick */
        break;

    /* ── 接收期：逐包寫入，逐包閒置逾時看門 ── */
    case OTA_STATE_RECV_DATA:
        if ((int32_t)(now - s_ctx.phase_deadline_ms) >= 0) {
            EnterError();
        }
        break;

    /* ── 接收期：本地 CRC 校驗（現已 inline 於 STORE handler，此態不再被進入）── */
    case OTA_STATE_VERIFY_LOCAL:
        if (FlashService_VerifyBankCRC(s_ctx.target_bank,
                                       s_ctx.fw_total_size,
                                       s_ctx.fw_expected_crc)) {
            SendUp(OTA_CMD_STATUS_RSP);
            s_ctx.state = OTA_STATE_IDLE;
        } else {
            EnterError();
        }
        break;

    /* ── 錯誤：通知上游，重置回 IDLE ── */
    case OTA_STATE_ERROR:
        SendUp(OTA_CMD_ERROR_RSP);
        s_ctx.state          = OTA_STATE_IDLE;
        s_ctx.current_offset = 0u;
        s_ctx.retry_count    = 0u;
        break;

    default:
        /* 轉發期狀態（FORWARD_*）由各端 Proxy 模組驅動，排程器不處理 */
        break;
    }
}

/* ────────────────────────────────────────────────────────
 *  OtaScheduler_OnReceivePacket
 *
 *  處理來自上游的 OTA 封包（CENTER→METER 或 HOST→CENTER 或 METER→RoomNode）。
 *  由各端的 dispatcher 呼叫，cmd 使用 OtaCommand_t 數值命名空間。
 * ──────────────────────────────────────────────────────── */
void OtaScheduler_OnReceivePacket(uint8_t cmd,
                                  const uint8_t *payload,
                                  uint16_t       len)
{
    if (payload == NULL) return;

    switch (cmd) {

    /* ── 1. 準備接收子節點韌體（統一 BL 格式）
     *    payload: [node_id 1B][payload_size 4B LE][fw_image_size 4B LE]
     *             [transport_crc32 4B LE][version 4B LE][meta_version 4B LE] = 21 bytes ── */
    case OTA_CMD_UPDATE_CHILD_REQ:
        if (s_ctx.state != OTA_STATE_IDLE) break;
        if (len < 21u) break;

        FlashService_EnsureOpen();

        s_ctx.target_node_id  = payload[0];
        /* payload_size → fw_total_size */
        s_ctx.fw_total_size   = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[3] << 16)
                              | ((uint32_t)payload[2] <<  8) |  (uint32_t)payload[1];
        /* fw_image_size */
        s_ctx.fw_image_size   = ((uint32_t)payload[8] << 24) | ((uint32_t)payload[7] << 16)
                              | ((uint32_t)payload[6] <<  8) |  (uint32_t)payload[5];
        /* transport_crc32 → fw_expected_crc */
        s_ctx.fw_expected_crc = ((uint32_t)payload[12] << 24) | ((uint32_t)payload[11] << 16)
                              | ((uint32_t)payload[10] <<  8) |  (uint32_t)payload[9];
        /* version */
        s_ctx.version         = ((uint32_t)payload[16] << 24) | ((uint32_t)payload[15] << 16)
                              | ((uint32_t)payload[14] <<  8) |  (uint32_t)payload[13];
        /* meta_version at payload[17..20] — validated but not stored */
        s_ctx.current_offset  = 0u;
        s_ctx.retry_count     = 0u;

        /* 基本合理性驗證：payload 大小必須非零、4-byte 對齊、不超過 Bank 容量 */
        if (s_ctx.fw_total_size == 0u
            || (s_ctx.fw_total_size & 3u) != 0u
            || s_ctx.fw_total_size > 0xE000UL) {   /* 56 KB bank 容量，對齊 flash_service BANK_SIZE */
            break;
        }

        /* 暫存 bank = 「執行中 bank 的相反邊」，避免覆寫正在執行的韌體。
         * active bank 以硬體 VECMAP 為準（FlashService_GetActiveBank），
         * fresh Keil flash（FW_Info 全 0xFF）也能正確判斷，不再因 FW_Info
         * 未初始化而靜默丟棄 UPDATE → relay 收不到 ACK 而傳不下去。 */
        {
            uint8_t running = FlashService_GetActiveBank();
            s_ctx.target_bank = (running == (uint8_t)FW_BANK_0) ? 1u : 0u;
        }

        s_ctx.state = OTA_STATE_RECV_INIT;
        break;

    /* ── 2. 子節點韌體資料 chunk（統一 BL 格式）
     *    payload: [chunk_offset 4B LE][data 最多 88B]，data 必須 4-byte 對齊 ── */
    case OTA_CMD_STORE_CHILD_REQ:
        if (s_ctx.state != OTA_STATE_RECV_DATA) break;
        if (len < 5u) break;                         /* 至少 offset(4) + 1 byte data */
        if (len > OTA_CHUNK_SIZE) break;
        if ((uint16_t)(len - 4u) & 3u) break;        /* data 必須 4-byte 對齊 */

        {
            uint32_t chunk_offset = ((uint32_t)payload[3] << 24) | ((uint32_t)payload[2] << 16)
                                   | ((uint32_t)payload[1] <<  8) |  (uint32_t)payload[0];
            uint16_t data_len     = (uint16_t)(len - 4u);
            /* 最後一包：上游固定送 92-byte 視窗，有效 data 可能少於 88；截斷至剩餘 bytes
             * （否則 current_offset+88>fw_total_size 會卡在結尾）*/
            {
                uint32_t remaining = s_ctx.fw_total_size - s_ctx.current_offset;
                if ((uint32_t)data_len > remaining)
                    data_len = (uint16_t)remaining;
            }

            if (chunk_offset != s_ctx.current_offset) break;   /* 不允許亂序 */

            if (FlashService_WriteFirmware(s_ctx.target_bank,
                                           s_ctx.current_offset,
                                           payload + 4u,
                                           data_len) != 0) {
                EnterError(); break;
            }
            s_ctx.current_offset += (uint32_t)data_len;
            /* 每收到一包就把 deadline 往後推：把「整包全域逾時」改成「逐包閒置逾時」，
             * 否則大韌體整段傳輸時間會超過 OTA_RECV_TIMEOUT_MS 而在傳到一半時被
             * Task() 誤判逾時 → EnterError。*/
            s_ctx.phase_deadline_ms = GetTick() + OTA_RECV_TIMEOUT_MS;

            if (s_ctx.current_offset < s_ctx.fw_total_size) {
                /* 傳輸中：回傳 rx_offset 進度 ACK */
                uint8_t ack[4];
                ack[0] = (uint8_t)(s_ctx.current_offset);
                ack[1] = (uint8_t)(s_ctx.current_offset >>  8);
                ack[2] = (uint8_t)(s_ctx.current_offset >> 16);
                ack[3] = (uint8_t)(s_ctx.current_offset >> 24);
                if (s_net && s_net->SendUpstream)
                    s_net->SendUpstream(OTA_CMD_STATUS_RSP, ack, 4u);
            } else {
                /* 全數接收：Inline transport CRC 驗證 */
                if (FlashService_VerifyBankCRC(s_ctx.target_bank,
                                               s_ctx.fw_total_size,
                                               s_ctx.fw_expected_crc)) {
                    uint8_t zero = 0x00;
                    if (s_net && s_net->SendUpstream)
                        s_net->SendUpstream(OTA_CMD_STATUS_RSP, &zero, 1u);
                    s_ctx.state = OTA_STATE_IDLE;  /* 等候 TRIGGER */
                } else {
                    EnterError();  /* Task() 下一 tick 送 ERROR_RSP */
                }
            }
        }
        break;

    /* ── 3. 觸發轉發給子節點
     *    本地 CRC 驗證通過後回 IDLE 等候此命令；
     *    透過 TriggerProxy 回呼委派 Proxy 模組驅動後續轉發
     *    （leaf 節點如 RoomNode：TriggerProxy 直接進 Bootloader，忽略 target_node_id）。── */
    case OTA_CMD_TRIGGER_CHILD_REQ:
        if (s_ctx.state != OTA_STATE_IDLE) break;
        if (s_ctx.fw_total_size == 0u) break;

        if (s_net && s_net->TriggerProxy) {
            s_net->TriggerProxy(s_ctx.target_bank,
                                s_ctx.fw_total_size,    /* payload_size */
                                s_ctx.fw_expected_crc,  /* transport_crc32 */
                                s_ctx.fw_image_size,
                                s_ctx.version,
                                s_ctx.target_node_id);
            s_ctx.fw_total_size = 0u;  /* 消費完畢，防止重複觸發 */
        }
        break;

    default:
        break;
    }
}

_Bool OtaScheduler_IsActive(void)
{
    return (s_ctx.state != OTA_STATE_IDLE && s_ctx.state != OTA_STATE_ERROR);
}
