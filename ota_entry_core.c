/* ota_entry_core.c — 共用 OTA 入口核心（介面與說明見 ota_entry_core.h）。
 * Flash 操作委派注入的 FlashService；平台原語委派注入的 IOtaEntrySys_t。 */
#include "ota_entry_core.h"
#include "flash_service.h"
#include "fw_info.h"
#include <stddef.h>

void OtaEntry_EnterBootloader(const IOtaEntrySys_t *sys)
{
    FW_Info_t info;

    FlashService_EnsureOpen();   /* 確保 FMC 已開啟（Init 在主迴圈前已注入驅動）*/

    if (FlashService_ReadFWInfo(&info) == 0)
    {
        info.cmd = (uint8_t)BTLD_ENTER_OTA;
        FlashService_UpdateFWInfo(&info);
    }

    if (sys && sys->WdtFeed)
        sys->WdtFeed();          /* 餵狗確保跳轉前不因 WDT 超時先死 */

    /* 關鍵：reset 前必須把 VECMAP remap 回 Bootloader(0x0)。
     * Cortex-M0 無 VTOR，跑哪個 bank 全靠 FMC VECMAP，且 VECMAP 跨 NVIC_SystemReset
     * 保留。若 App 正跑在 Bank1(VECMAP=0x10000)時直接系統重置，reset 後 CPU 讀 0x0
     * 仍被 remap 到 0x10000 → 又跑 Bank1 App 而非 BL → BTLD_ENTER_OTA 永遠不被處理
     * →「已更新的裝置無法再更新」。FlashService_JumpToBootloader() =
     * remap 回 0x0 + reset（不返回）。 */
    FlashService_JumpToBootloader();

    /* 不可達後備：JumpToBootloader 已 reset */
    if (sys && sys->SystemReset)
        sys->SystemReset();
    while (1) {}
}

/* 冪等快取：確認過一次後即短路（單例操作，file-static 合理）。 */
static uint8_t s_health_confirmed = 0u;

void OtaEntry_ConfirmHealth(const IOtaEntrySys_t *sys)
{
    FW_Info_t       info;
    Bank_MetaInfo_t meta;
    uint8_t         bank;

    (void)sys;   /* 健康確認不需平台原語；保留參數以對齊 EnterBootloader 形狀 */

    if (s_health_confirmed)
        return;

    FlashService_EnsureOpen();

    if (FlashService_ReadFWInfo(&info) != 0)
        return;

    bank = info.active_bank;
    if (bank > 1u)
        bank = 1u;   /* 非法值退回 Bank1（index 1）*/

    if (FlashService_ReadBankMeta(bank, &meta) != 0)
        return;

    if (meta.health != (uint8_t)FW_HEALTH_CONFIRMED)
    {
        meta.health        = (uint8_t)FW_HEALTH_CONFIRMED;
        meta.trial_counter = 0u;
        FlashService_UpdateBankMeta(bank, &meta);
    }

    s_health_confirmed = 1u;
}
