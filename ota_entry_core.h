#ifndef OTA_ENTRY_CORE_SHARED_H
#define OTA_ENTRY_CORE_SHARED_H

#include <stdint.h>

/*
 * 共用 OTA 入口核心（Single Source of Truth）
 * ------------------------------------------------------------------
 * 「App 觸發進入 Bootloader」與「開機後確認韌體健康」兩段業務邏輯，原本在
 * Gateway / Concentrator / RoomNode 三節點各抄一份（逐行相同，只差平台原語）。
 * 此處收斂成單一實作：Flash 操作走已注入的 FlashService（共用），平台相依的
 * 「餵狗 / 系統重置」鎖進注入的 IOtaEntrySys_t vtable。
 *
 * 各節點 services/ota_entry.c 退成薄 adapter：填一份 IOtaEntrySys_t、把公開的
 * OTA_EnterBootloader / OTA_ConfirmHealth 轉呼叫到此處（與 flash_service /
 * ota_scheduler 的 shim 形狀一致）。
 *
 * 前置條件：呼叫前 FlashService_Init(driver) 必須已執行（s_fmc 已注入）。
 * 三節點皆在主迴圈啟動前完成注入，故 EnterBootloader 內以 EnsureOpen() 即可。
 *
 * 工具鏈：純 C，需同時通過 Keil armclang 與 Atmel Studio arm-gcc。
 */

/* ── 平台系統原語介面（每節點一份 adapter）── */
typedef struct {
    void (*WdtFeed)(void);     /* 餵狗，避免重置前先被 WDT 咬死 */
    void (*SystemReset)(void); /* 系統重置；不返回（僅作 JumpToBootloader 的後備）*/
} IOtaEntrySys_t;

/*
 * OtaEntry_EnterBootloader
 *   讀 FW_Info → cmd = BTLD_ENTER_OTA → 寫回 → 跳回 Bootloader（含 VECMAP remap）。
 *   不返回。sys 可為 NULL（則略過餵狗 / 後備重置）。
 */
void OtaEntry_EnterBootloader(const IOtaEntrySys_t *sys);

/*
 * OtaEntry_ConfirmHealth
 *   將目前執行 Bank 標記為 FW_HEALTH_CONFIRMED 並清 trial_counter，
 *   防止 Bootloader 下次重啟誤判健康失敗而 Rollback。
 *   冪等：確認過一次後即快取，後續直接返回，不再讀寫 Flash。
 */
void OtaEntry_ConfirmHealth(const IOtaEntrySys_t *sys);

#endif /* OTA_ENTRY_CORE_SHARED_H */
