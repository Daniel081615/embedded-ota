# Wire protocol & on-flash layout

Everything here is "must agree across devices". Transport framing (start byte,
checksum, terminator) is *your* adapter's job — the core deals only in
`(cmd, payload, len)` and the encodings below.

## OTA commands (`OtaCommand_t`)

| Value | Name | Direction | Meaning |
|---|---|---|---|
| `0x20` | `ENTER_REQ` | up → down | request enter OTA mode |
| `0x21` | `STATUS_RSP` | down → up | progress / CRC-OK / awaiting trigger |
| `0x22` | `UPDATE_LOCAL_REQ` | up → self | update this node |
| `0x23` | `UPDATE_CHILD_REQ` | up → self | prepare to receive a child image |
| `0x24` | `STORE_CHILD_REQ` | up → self | one firmware chunk |
| `0x25` | `TRIGGER_CHILD_REQ` | up → self | begin forward / boot |
| `0x26` | `APP_READY` | down → up | child's new app booted |
| `0x40` | `ENTER_RSP` | down → up | ENTER / chunk ACK |
| `0xFF` | `ERROR_RSP` | any | error |

## `UPDATE_CHILD_REQ` payload

The scheduler expects a **node-id prefix + the canonical 20-byte `UPDATE_META`**
(21 bytes total):

```
[0]      node_id
[1..4]   payload_size     (LE)   bytes to be received (4-byte aligned, <= bank size)
[5..8]   fw_image_size    (LE)   raw firmware size
[9..12]  transport_crc32  (LE)   CRC32 over payload, verified after store
[13..16] version          (LE)
[17..20] meta_version     (LE)   = 1
```

Build the 20-byte body with the provided encoder, then prepend the node id:

```c
uint8_t meta[1 + OTA_PROTO_UPDATE_META_LEN];   /* 1 + 20 */
meta[0] = child_node_id;
OtaProto_EncodeUpdateMeta(&meta[1], payload_size, fw_image_size, crc32, version);
```

## `STORE_CHILD_REQ` payload

```
[0..3]   chunk_offset (LE)
[4..]    firmware data, 4-byte aligned, up to 88 bytes
```

- Total `len` must be `5 .. OTA_CHUNK_SIZE`.
- `chunk_offset` must equal the receiver's current offset (in-order streaming).
  An offset *behind* current is treated as a lost-ACK retransmit (re-ACKed, not
  re-written); an offset *ahead* is a protocol error.
- The final chunk may carry fewer than 88 valid bytes; the receiver truncates to
  the remaining size.

`OTA_CHUNK_SIZE` (default 92) is the max chunk payload and is tied to the
reference 100-byte UART frame (`[start][id][cmd][payload≤92][checksum][term]`).
Change it to match your transport — see the porting guide.

## On-flash layout (`fw_info.h`)

Shared with the bootloader, `#pragma pack(1)`, sizes locked by `_Static_assert`
(needs `-fshort-enums`).

```c
typedef struct {            /* 8 bytes */
    FW_BankId_t    active_bank;   /* 0 / 1 */
    FW_BtldCmd_t   cmd;           /* controls bootloader on next reset */
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
    uint32_t       fw_size;       /* raw firmware bytes */
    uint32_t       fw_crc32;      /* post-stage CRC32 */
} Bank_MetaInfo_t;
```

Key enum values: `FW_BtldCmd_t` (`BTLD_CMD_NONE=0xFF`, `BTLD_ENTER_OTA=0xA1`, …),
`FW_BankUsage_t` (`EMPTY=0xFF`, `ACTIVE=0x01`, `VALID=0x06`, …),
`FW_Health_t` (`UNVERIFIED=0x00`, `CONFIRMED=0xA5`). `0xFF` defaults match the
erased-flash state on purpose.
