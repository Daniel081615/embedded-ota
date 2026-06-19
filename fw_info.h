#ifndef FW_INFO_SHARED_H
#define FW_INFO_SHARED_H

#include <stdint.h>

/*
 * 共用 OTA 韌體資訊契約（Single Source of Truth）
 * ------------------------------------------------------------------
 * 由 Bootloader / CENTER / METER / READER 四端共用，描述存於各平台
 * Data Flash 的 FW_Info / Bank_MetaInfo 佈局與所有 enum 值。
 *
 * 「跨裝置必須一致」的東西放這裡；平台相依的位址（FW_INFO_BASE 等）
 * 不在此檔——各專案於自己的 fw_info.h shim 內依硬體定義。
 *
 * 修改規則：任何一方變更 = 變更全體。structs 以 #pragma pack(1) 確保
 * 無 padding，並用 static-assert 守住大小（避免 enum 被當 4-byte 時
 * 在實機上靜默寫壞 Data Flash）。
 *
 * 工具鏈：本檔需同時通過 Keil armclang 與 Atmel Studio arm-gcc，
 * 僅用純 C，勿加編譯器專屬語法。
 */

/* Bank ID: 0-indexed to match BSP_BANK0_BASE / BSP_BANK1_BASE */
typedef enum {
    FW_BANK_0       = 0,    /* Bank 0 = APROM 0x2000 */
    FW_BANK_1       = 1,    /* Bank 1 = APROM 0x10000 */
    FW_BANK_INVALID = 0xFF,
} FW_BankId_t;

/* Bootloader command: 控制下一次 reset 時 Bootloader 行為 */
typedef enum {
    BTLD_CMD_NONE     = 0xFF, /* 正常開機：依 active_bank / bank_usage 選擇 Bank 跳入 */
    BTLD_ENTER_OTA    = 0xA1, /* App 觸發：要求 Bootloader 進入 OTA 接收主迴圈（原 BTLD_UPDATE_METER，值不變）*/
    BTLD_PATCH        = 0xA2, /* BL 直接對 INCOMING bank 執行 offset patch，跳過接收 */
    BTLD_FORCE_BANK1  = 0x11, /* 強制跳入 Bank 0（維修用） */
    BTLD_FORCE_BANK2  = 0x12, /* 強制跳入 Bank 1（維修用） */
} FW_BtldCmd_t;

/*
 * Bank 用途（Usage）— 描述 bank 目前的角色，非狀態機。
 * EMPTY = 0xFF 與 Flash 擦除後的預設值一致。
 */
typedef enum {
    BANK_USAGE_EMPTY        = 0xFF, /* 未使用 / Flash 預設狀態 */
    BANK_USAGE_ACTIVE       = 0x01, /* 目前執行中的正式版本（App 確認健康後寫入） */
    BANK_USAGE_PREV         = 0x02, /* 上一版，保留作 rollback */
    BANK_USAGE_INCOMING     = 0x03, /* 正在接收 / 處理中（patch 前暫態） */
    BANK_USAGE_RELAY_METER  = 0x04, /* 暫存 METER 韌體 .bin */
    BANK_USAGE_RELAY_READER = 0x05, /* 暫存 READER 韌體 .bin */
    BANK_USAGE_VALID        = 0x06, /* OTA 接收完成且 CRC 驗證通過，等待 App 確認 */
} FW_BankUsage_t;

/* 韌體健康狀態 */
typedef enum {
    FW_HEALTH_UNVERIFIED = 0x00, /* 新版本尚未通過 App 端健康確認流程 */
    FW_HEALTH_UNKNOWN    = 0x00, /* 同 UNVERIFIED（別名） */
    FW_HEALTH_CONFIRMED  = 0xA5, /* App 已確認此版本穩定可用 */
} FW_Health_t;

#pragma pack(push, 1)

/* Data Flash 中的共用 FW_Info 結構（單一一份, 8 bytes） */
typedef struct {
    FW_BankId_t    active_bank;   /* 0=Bank0, 1=Bank1 */
    FW_BtldCmd_t   cmd;           /* 控制 Bootloader 特殊行為 */
    FW_BankUsage_t bank0_usage;   /* Bank0 目前的用途 */
    FW_BankUsage_t bank1_usage;   /* Bank1 目前的用途 */
    FW_Health_t    health;        /* ACTIVE 版本是否已被 App 確認健康 */
    uint8_t        trial_counter; /* 未確認健康狀態下的開機次數（BL 用於判斷 rollback） */
    uint8_t        reserved[2];
} FW_Info_t;                      /* 8 bytes */

/* Bank 元資料結構 (16 bytes) */
typedef struct {
    FW_BankUsage_t usage;
    FW_Health_t    health;
    uint8_t        trial_counter;
    uint8_t        reserved;
    uint32_t       version;
    uint32_t       fw_size;   /* raw 韌體大小（bytes，不含 OTA metadata page） */
    uint32_t       fw_crc32;  /* post-patch CRC32（由 ota_offset_patcher 計算後寫入） */
} Bank_MetaInfo_t;             /* 16 bytes */

#pragma pack(pop)

/* 編譯期守住 Data Flash 佈局：若 enum 未以 1-byte 編碼（未開 -fshort-enums）
 * 會在此 build 失敗，而不是在實機上靜默寫壞 Data Flash。 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(FW_Info_t)      ==  8, "FW_Info_t must be 8 bytes (check -fshort-enums)");
_Static_assert(sizeof(Bank_MetaInfo_t) == 16, "Bank_MetaInfo_t must be 16 bytes (check -fshort-enums)");
#endif

/* Helper 宏 */
#define FW_INFO_ACTIVE_BANK(info)  ((FW_BankId_t)((info)->active_bank))
#define FW_INFO_CMD(info)          ((FW_BtldCmd_t)((info)->cmd))
#define FW_INFO_HEALTH(info)       ((FW_Health_t)((info)->health))

#endif /* FW_INFO_SHARED_H */
