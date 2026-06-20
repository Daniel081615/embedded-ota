#ifndef OTA_FLASH_PORT_H
#define OTA_FLASH_PORT_H

#include <stdint.h>

/*
 * OTA flash port 介面（Single Source of Truth）
 * ------------------------------------------------------------------
 * ota 自帶的 flash 驅動契約，使 Shared/ota 不再 include 任何平台 HAL header
 * （達到「copy 即可獨立編譯」）。平台實作此 vtable（填 8 個函式指標）後，
 * 經 FlashService_Init() 注入；flash_service.c 本身不碰暫存器。
 *
 * 幾何常數為 NUC1261 預設，非此佈局之平台以
 * -DOTA_FLASH_PAGE_SIZE / -DOTA_FLASH_BANK_SIZE 覆寫（不必改本檔）。
 *
 * 回傳碼約定：int32_t 一律 0 = OK、負 = 錯。
 *
 * 工具鏈：純 C，需同時通過 Keil armclang 與 Atmel Studio arm-gcc。
 */

/* ── flash 幾何（可 -D 覆寫）── */
#ifndef OTA_FLASH_PAGE_SIZE
#define OTA_FLASH_PAGE_SIZE  (0x0800UL)   /* 2 KB logical page（NUC1261 預設）*/
#endif
#ifndef OTA_FLASH_BANK_SIZE
#define OTA_FLASH_BANK_SIZE  (0xE000UL)   /* 56 KB per OTA bank（NUC1261 預設）*/
#endif

/* ── flash 驅動 vtable（int32_t: 0 = OK, 負 = 錯）── */
typedef struct {
    int32_t  (*Init)(void);
    void     (*DeInit)(void);
    int32_t  (*ErasePage)(uint32_t page_addr);
    int32_t  (*WriteWords)(uint32_t addr, const uint32_t *data, uint32_t word_count);
    void     (*ReadWords)(uint32_t addr, uint32_t *data, uint32_t word_count);
    uint32_t (*GetCRC32)(uint32_t addr, uint32_t byte_len);
    void     (*JumpToApp)(uint32_t addr);
    uint8_t  (*GetActiveBank)(void);   /* 回傳目前執行中的 bank（VECMAP / VTOR 來源）*/
} IFmcDriver_t;

#endif /* OTA_FLASH_PORT_H */
