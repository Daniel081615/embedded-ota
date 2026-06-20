[English](wire-protocol.md) | **繁體中文**

# Wire 協定與 on-flash 佈局

這裡的東西都是「跨裝置必須一致」。傳輸層的框(起始 byte、checksum、終止)是**你的 adapter** 的事——
核心只處理 `(cmd, payload, len)` 與下列編碼。

## OTA 指令(`OtaCommand_t`)

| 值 | 名稱 | 方向 | 意義 |
|---|---|---|---|
| `0x20` | `ENTER_REQ` | 上 → 下 | 請求進入 OTA 模式 |
| `0x21` | `STATUS_RSP` | 下 → 上 | 進度 / CRC-OK / 等待觸發 |
| `0x22` | `UPDATE_LOCAL_REQ` | 上 → 自 | 更新本節點 |
| `0x23` | `UPDATE_CHILD_REQ` | 上 → 自 | 準備接收子節點映像 |
| `0x24` | `STORE_CHILD_REQ` | 上 → 自 | 一個韌體 chunk |
| `0x25` | `TRIGGER_CHILD_REQ` | 上 → 自 | 開始轉發 / 開機 |
| `0x26` | `APP_READY` | 下 → 上 | 子節點新 App 已開機 |
| `0x40` | `ENTER_RSP` | 下 → 上 | ENTER / chunk ACK |
| `0xFF` | `ERROR_RSP` | 任意 | 錯誤 |

## `UPDATE_CHILD_REQ` payload

scheduler 預期 **node-id 前綴 + 標準 20-byte `UPDATE_META`**(共 21 bytes):

```
[0]      node_id
[1..4]   payload_size     (LE)   要接收的 bytes(4-byte 對齊,≤ bank 容量)
[5..8]   fw_image_size    (LE)   原始韌體大小
[9..12]  transport_crc32  (LE)   payload 的 CRC32,存完後驗證
[13..16] version          (LE)
[17..20] meta_version     (LE)   = 1
```

用提供的編碼器產生 20-byte 本體,再前綴 node id:

```c
uint8_t meta[1 + OTA_PROTO_UPDATE_META_LEN];   /* 1 + 20 */
meta[0] = child_node_id;
OtaProto_EncodeUpdateMeta(&meta[1], payload_size, fw_image_size, crc32, version);
```

## `STORE_CHILD_REQ` payload

```
[0..3]   chunk_offset (LE)
[4..]    韌體資料,4-byte 對齊,最多 88 bytes
```

- 總 `len` 必須 `5 .. OTA_CHUNK_SIZE`。
- `chunk_offset` 必須等於接收端目前的 offset(順序串流)。offset **落後**目前值視為
  ACK 遺失的重傳(重 ACK,不重寫);offset **超前**則是協定錯誤。
- 最後一包有效 bytes 可能少於 88;接收端會截斷至剩餘大小。

`OTA_CHUNK_SIZE`(預設 92)是 chunk payload 上限,綁的是參考用的 100-byte UART frame
(`[起始][id][cmd][payload≤92][checksum][終止]`)。換傳輸層時改它——見移植指南。

## On-flash 佈局(`fw_info.h`)

與 Bootloader 共用,`#pragma pack(1)`,大小由 `_Static_assert` 鎖住(需 `-fshort-enums`)。

```c
typedef struct {            /* 8 bytes */
    FW_BankId_t    active_bank;   /* 0 / 1 */
    FW_BtldCmd_t   cmd;           /* 控制下次 reset 時 Bootloader 行為 */
    FW_BankUsage_t bank0_usage;
    FW_BankUsage_t bank1_usage;
    FW_Health_t    health;
    uint8_t        trial_counter;
    uint8_t        reserved[2];
} FW_Info_t;

typedef struct {            /* 16 bytes */
    FW_BankUsage_t usage;
    FW_Health_t    health;
    uint8_t        trial_counter;
    uint8_t        reserved;
    uint32_t       version;
    uint32_t       fw_size;       /* 原始韌體 bytes */
    uint32_t       fw_crc32;      /* staging 後的 CRC32 */
} Bank_MetaInfo_t;
```

關鍵 enum 值:`FW_BtldCmd_t`(`BTLD_CMD_NONE=0xFF`、`BTLD_ENTER_OTA=0xA1`…)、
`FW_BankUsage_t`(`EMPTY=0xFF`、`ACTIVE=0x01`、`VALID=0x06`…)、
`FW_Health_t`(`UNVERIFIED=0x00`、`CONFIRMED=0xA5`)。`0xFF` 預設刻意對齊 flash 擦除後的狀態。
