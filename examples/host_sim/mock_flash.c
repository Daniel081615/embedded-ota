/* examples/host_sim/mock_flash.c — RAM-backed IFmcDriver_t for host testing. */
#include "mock_flash.h"
#include <string.h>

/* 128 KB covers the NUC1261 reference layout up to FW_INFO_BASE (0x1F800). */
#define MOCK_FLASH_SIZE   0x20000u

static uint8_t s_flash[MOCK_FLASH_SIZE];

uint32_t MockFlash_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int k;
    for (i = 0u; i < len; i++) {
        crc ^= data[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

void MockFlash_Reset(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
}

static int32_t fm_init(void)   { return 0; }
static void    fm_deinit(void) { }

static int32_t fm_erase_page(uint32_t addr)
{
    if (addr + OTA_FLASH_PAGE_SIZE > MOCK_FLASH_SIZE) return -1;
    memset(&s_flash[addr], 0xFF, OTA_FLASH_PAGE_SIZE);
    return 0;
}

static int32_t fm_write_words(uint32_t addr, const uint32_t *data, uint32_t word_count)
{
    if (addr + word_count * 4u > MOCK_FLASH_SIZE) return -1;
    memcpy(&s_flash[addr], data, word_count * 4u);
    return 0;
}

static void fm_read_words(uint32_t addr, uint32_t *data, uint32_t word_count)
{
    memcpy(data, &s_flash[addr], word_count * 4u);
}

static uint32_t fm_get_crc32(uint32_t addr, uint32_t byte_len)
{
    return MockFlash_Crc32(&s_flash[addr], byte_len);
}

static void    fm_jump_to_app(uint32_t addr) { (void)addr; /* host: no-op */ }
static uint8_t fm_get_active_bank(void)      { return 0u;  /* pretend running Bank 0 */ }

const IFmcDriver_t MOCK_FLASH = {
    fm_init, fm_deinit, fm_erase_page, fm_write_words,
    fm_read_words, fm_get_crc32, fm_jump_to_app, fm_get_active_bank
};
