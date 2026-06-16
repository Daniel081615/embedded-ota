/* flash_service.c — 共用 OTA Flash 服務層（CENTER/METER，NUC1261）
 * 介面與說明見 flash_service.h。以「修好的 METER 版」為基準。 */
#include "flash_service.h"
#include <stddef.h>
#include <string.h>

/* ── Flash 幾何 ──
 * page / bank size 取自平台 HAL（單一真相，避免重複定義漂移）。 */
#define PAGE_SIZE  HAL_FLASH_PAGE_SIZE   /* NUC1261: 2048 */
#define BANK_SIZE  HAL_FLASH_BANK_SIZE   /* 56 KB（0xE000）：對齊 BSP_BANK_SIZE。
                                          * 舊值 60 KB 會讓 EraseBank 多抹 2 頁 →
                                          * Bank0 抹除時越界蓋到 Bank1。 */

/* Bank / Meta / FW_Info 起始位址（NUC1261 預設，對齊 bsp_flash.h 的 BSP_*；
 * 非 NUC1261 平台以 -D 覆寫）。 */
#ifndef FS_BANK0_BASE
#define FS_BANK0_BASE       0x00002000UL   /* = BSP_BANK0_BASE */
#endif
#ifndef FS_BANK1_BASE
#define FS_BANK1_BASE       0x00010000UL   /* = BSP_BANK1_BASE */
#endif
#ifndef FS_FW_INFO_BASE
#define FS_FW_INFO_BASE     0x0001F800UL   /* = BSP_FW_INFO_BASE（Data Flash）*/
#endif
#ifndef FS_BANK0_META_BASE
#define FS_BANK0_META_BASE  0x0000F800UL   /* = BSP_BANK0_META_BASE（Bank0 末端 page）*/
#endif
#ifndef FS_BANK1_META_BASE
#define FS_BANK1_META_BASE  0x0001D800UL   /* = BSP_BANK1_META_BASE（Bank1 末端 page）*/
#endif

/* 靜態指標：保存注入的硬體驅動 (Injected Hardware Driver) */
static const IFmcDriver_t *s_fmc = NULL;
/* 整頁緩衝區改為靜態變數（.bss），避免 2KB 放在 1KB 堆疊上造成 stack overflow → HardFault */
static uint32_t s_page_buffer[PAGE_SIZE / 4];

/* 私有函式：取得指定 Bank 的起始位址 */
static uint32_t GetBankBaseAddr(uint8_t bank_index) {
    return (bank_index == 0) ? FS_BANK0_BASE : FS_BANK1_BASE;
}

/* 私有函式：取得指定 Bank 的詮釋資料位址 */
static uint32_t GetMetaBaseAddr(uint8_t bank_index) {
    return (bank_index == 0) ? FS_BANK0_META_BASE : FS_BANK1_META_BASE;
}

int32_t FlashService_Init(const IFmcDriver_t *driver) {
    if (!driver || !driver->ErasePage || !driver->WriteWords) return -1;
    s_fmc = driver;
    if (s_fmc->Init) return s_fmc->Init();
    return 0;
}

int32_t FlashService_EnsureOpen(void) {
    if (!s_fmc || !s_fmc->Init) return 0;
    return s_fmc->Init();
}

int32_t FlashService_EraseBank(uint8_t bank_index) {
    if (!s_fmc) return -1;
    uint32_t base_addr = GetBankBaseAddr(bank_index);
    uint32_t pages = BANK_SIZE / PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++) {
        if (s_fmc->ErasePage(base_addr + (i * PAGE_SIZE)) != 0) {
            return -2; /* 擦除失敗 (Erase Failed) */
        }
    }
    return 0;
}

int32_t FlashService_WriteFirmware(uint8_t bank_index, uint32_t offset, const uint8_t *payload, uint16_t byte_len) {
    if (!s_fmc) return -1;
    if (byte_len % 4 != 0) return -3;

    uint32_t target_addr = GetBankBaseAddr(bank_index) + offset;
    uint32_t word_count = byte_len / 4;

    /* 注意：呼叫端必須保證 payload 的記憶體位址是 4-byte 對齊的 */
    return s_fmc->WriteWords(target_addr, (const uint32_t *)payload, word_count);
}

static int32_t ModifyPageData(uint32_t target_addr, const void *new_data, uint32_t data_len) {
    if (!s_fmc) return -1;
    if (!new_data || data_len == 0) return -4; /* 安全檢查：空指標 / 長度 0 */

    uint32_t page_addr = target_addr & ~(PAGE_SIZE - 1);
    uint32_t offset = target_addr - page_addr;

    /* 邊界檢查：避免 memcpy 寫出單一頁面範圍 */
    if ((offset + data_len) > PAGE_SIZE) {
        return -5;
    }

    /* 1. 讀取原頁面資料 */
    s_fmc->ReadWords(page_addr, s_page_buffer, PAGE_SIZE / 4);
    /* 2. 修改記憶體中的資料 */
    memcpy((uint8_t*)s_page_buffer + offset, new_data, data_len);
    /* 3. 擦除頁面 */
    if (s_fmc->ErasePage(page_addr) != 0) return -2;
    /* 4. 寫回資料 */
    if (s_fmc->WriteWords(page_addr, s_page_buffer, PAGE_SIZE / 4) != 0) return -3;

    return 0;
}

/* ================= Read API ================= */

int32_t FlashService_ReadFirmware(uint8_t bank_index, uint32_t offset, uint8_t *buf, uint16_t byte_len) {
    if (!s_fmc || !buf) return -1;
    /* byte_len 需 4-byte 對齊（NUC1261 FMC 限制） */
    if (byte_len % 4u != 0u) return -3;

    uint32_t target_addr = GetBankBaseAddr(bank_index) + offset;
    s_fmc->ReadWords(target_addr, (uint32_t *)buf, byte_len / 4u);
    return 0;
}

int32_t FlashService_ReadFWInfo(FW_Info_t *info) {
    if (!s_fmc || !info) return -1;
    s_fmc->ReadWords(FS_FW_INFO_BASE, (uint32_t *)info, sizeof(FW_Info_t) / 4);
    return 0;
}

int32_t FlashService_ReadBankMeta(uint8_t bank_index, Bank_MetaInfo_t *meta) {
    if (!s_fmc || !meta) return -1;
    uint32_t target_addr = GetMetaBaseAddr(bank_index);
    s_fmc->ReadWords(target_addr, (uint32_t *)meta, sizeof(Bank_MetaInfo_t) / 4);
    return 0;
}

/* ================= 保持原有的 Update API ================= */

int32_t FlashService_UpdateFWInfo(const FW_Info_t *info) {
    return ModifyPageData(FS_FW_INFO_BASE, info, sizeof(FW_Info_t));
}

int32_t FlashService_UpdateBankMeta(uint8_t bank_index, const Bank_MetaInfo_t *meta) {
    uint32_t target_addr = GetMetaBaseAddr(bank_index);
    return ModifyPageData(target_addr, meta, sizeof(Bank_MetaInfo_t));
}

uint8_t FlashService_VerifyBankCRC(uint8_t  bank_index,
                                   uint32_t fw_size,
                                   uint32_t expected_crc)
{
    uint32_t base;
    uint32_t crc_size;
    uint32_t actual_crc;

    if (!s_fmc || !s_fmc->GetCRC32) return 0u;

    /* fw_size 需 4-byte 對齊且不超過 bank 容量；不合法時退化為全 Bank CRC */
    if (fw_size == 0u || fw_size > BANK_SIZE || (fw_size & 3u) != 0u)
        crc_size = BANK_SIZE;
    else
        crc_size = fw_size;

    base = GetBankBaseAddr(bank_index);
    actual_crc = s_fmc->GetCRC32(base, crc_size);

    return (actual_crc == expected_crc) ? 1u : 0u;
}

void FlashService_JumpToApp(uint32_t app_base) {
    if (s_fmc && s_fmc->JumpToApp) {
        s_fmc->JumpToApp(app_base); /* 把跳轉工作委託給底層硬體驅動 */
    }
}

uint8_t FlashService_GetActiveBank(void) {
    if (s_fmc && s_fmc->GetActiveBank) {
        return s_fmc->GetActiveBank();
    }
    return 0u; /* 無驅動時回退 Bank0 */
}
