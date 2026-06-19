#ifndef FLASH_SERVICE_SHARED_H
#define FLASH_SERVICE_SHARED_H

#include <stdint.h>
#include "fw_info.h"          /* FW_Info_t / Bank_MetaInfo_t（共用契約，types-only）*/
#include "HAL/hal_flash.h"    /* HAL_FlashDriver_t（平台 HAL，含 flash 幾何常數）*/

/*
 * 共用 OTA Flash 服務層（Single Source of Truth）
 * ------------------------------------------------------------------
 * Bank / Meta / Firmware staging 邏輯（Layer C）。透過注入的
 * IFmcDriver_t（= 平台 HAL_FlashDriver_t vtable）操作硬體，本身不碰暫存器。
 *
 * 由 CENTER(Gateway) / METER(Concentrator)（皆 NUC1261）共用；實作以
 * 「修好的 METER 版」為基準（VECMAP active-bank 判斷 + static page buffer +
 * BANK_SIZE 0xE000，避免越界抹除與 2KB stack 溢位 HardFault）。
 *
 * 幾何（page/bank size）取自平台 HAL_FLASH_* 常數；FW_Info / bank 起始位址
 * 為 NUC1261 預設，於 flash_service.c 內以 FS_* #ifndef 提供，非 NUC1261
 * 平台可 -D 覆寫。
 */

/* ── 1. 硬體驅動介面 (Hardware Driver Interface) ── */
typedef HAL_FlashDriver_t IFmcDriver_t;  /* portable alias */

/* ── 2. 服務層初始化 (Service Initialization) ── */
int32_t FlashService_Init(const IFmcDriver_t *driver);
int32_t FlashService_EnsureOpen(void);

/* ── 3. 韌體操作 (Firmware Operations) ── */
int32_t FlashService_EraseBank(uint8_t bank_index);
int32_t FlashService_WriteFirmware(uint8_t bank_index, uint32_t offset, const uint8_t *payload, uint16_t byte_len);
int32_t FlashService_ReadFirmware(uint8_t bank_index, uint32_t offset, uint8_t *buf, uint16_t byte_len);
uint8_t FlashService_VerifyBankCRC(uint8_t bank_index,
                                   uint32_t fw_size,
                                   uint32_t expected_crc);

/* ── 4. 詮釋資料管理 (Metadata Management) ── */
int32_t FlashService_ReadFWInfo(FW_Info_t *info);
int32_t FlashService_UpdateFWInfo(const FW_Info_t *info);
int32_t FlashService_ReadBankMeta(uint8_t bank_index, Bank_MetaInfo_t *meta);
int32_t FlashService_UpdateBankMeta(uint8_t bank_index, const Bank_MetaInfo_t *meta);

/* ── 5. 系統啟動與跳轉 ── */
void FlashService_JumpToApp(uint32_t app_base);

/* 跳回 Bootloader（APROM 0x0）。封裝 BL 基底位址，呼叫端（ota_entry）無需知道任何
 * 平台常數。NUC1261 預設 0x0；非此佈局平台以 -DFS_BOOTLOADER_BASE 覆寫。
 * 於 Cortex-M0 同時負責把 VECMAP remap 回 0x0（由底層 JumpToApp 驅動完成）；不返回。 */
void FlashService_JumpToBootloader(void);

/* 回傳目前實際執行中的 Bank（0/1），以硬體 VECMAP 為準。
 * fresh Keil flash（FW_Info 未初始化）也能正確判斷；無驅動時回退 Bank0。 */
uint8_t FlashService_GetActiveBank(void);

#endif /* FLASH_SERVICE_SHARED_H */
