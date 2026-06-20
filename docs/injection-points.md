**English** | [繁體中文](injection-points.zh-TW.md)

# Injection points

Every platform dependency is a vtable you fill in and inject. The core holds the
pure-C state machines; hardware differences live behind function pointers.
**Return-code convention: `int32_t` is `0` = OK, negative = error.**

---

## 1. `IFmcDriver_t` — flash driver  ★most critical

Defined in `ota_flash_port.h`. The only layer that touches registers — **unit-test
it first** (erase a page → write words → read back → check `GetCRC32`).

```c
typedef struct {
    int32_t  (*Init)(void);
    void     (*DeInit)(void);
    int32_t  (*ErasePage)(uint32_t page_addr);
    int32_t  (*WriteWords)(uint32_t addr, const uint32_t *data, uint32_t word_count);
    void     (*ReadWords)(uint32_t addr, uint32_t *data, uint32_t word_count);
    uint32_t (*GetCRC32)(uint32_t addr, uint32_t byte_len);
    void     (*JumpToApp)(uint32_t addr);
    uint8_t  (*GetActiveBank)(void);
} IFmcDriver_t;
```

- **Units**: `WriteWords` / `ReadWords` take a **word (4-byte) count**, not bytes.
- **Alignment**: `ErasePage(addr)` must be page-aligned; `WriteWords` start usually
  word/double-word aligned (MCU-dependent).
- `GetActiveBank()`: which bank is *currently executing* — Cortex-M0 via the
  vendor VECMAP, M0+/M3/M4 via VTOR or a stored flag. Must return a safe fallback
  (Bank 0) on fresh flash.
- `JumpToApp(addr)`: set the vector table relocation, then jump; **does not return**.

Inject with `FlashService_Init(&driver)` before the main loop.
Mock reference: [`examples/host_sim/mock_flash.c`](../examples/host_sim/mock_flash.c).

---

## 2. `IOtaSys_t` — millisecond tick

```c
typedef struct {
    uint32_t (*GetTickMs)(void);   /* monotonic ms; 32-bit wrap handled via unsigned diff */
} IOtaSys_t;
```

All timeouts use `(int32_t)(now - deadline) >= 0`, so counter wrap needs no special
handling. Wire it to your SysTick / 1 ms counter.

---

## 3. `IOtaNet_t` — receive / send (used by `ota_scheduler`)

```c
typedef struct {
    int32_t (*SendUpstream)(uint8_t cmd, const uint8_t *payload, uint16_t len);
    int32_t (*SendDownstream)(uint8_t node_id, uint8_t cmd, const uint8_t *payload, uint16_t len);
    void    (*TriggerProxy)(uint8_t bank_index, uint32_t payload_size, uint32_t transport_crc32,
                            uint32_t fw_image_size, uint32_t version, uint8_t target_node_id);
} IOtaNet_t;
```

- `SendUpstream`: send ACK / status back to the source (leaf nodes mainly use this).
- `SendDownstream`: only chain nodes; a leaf can leave it a no-op.
- `TriggerProxy`: called by the scheduler after a full image is stored **and** CRC
  passes. For a **leaf**, this jumps to the bootloader (`target_node_id` ignored);
  for a **chain node**, it delegates to `OtaForwarder_Trigger`.
- Your transport ISR decodes a frame and calls
  `OtaScheduler_OnReceivePacket(cmd, payload, len)` — where **`len` must equal the
  actual chunk length** (≤ `OTA_CHUNK_SIZE`); see the pitfalls doc.

Inject with `OtaScheduler_Init(role, &net, &sys)`.

---

## 4. `IOtaEntrySys_t` — bootloader-entry primitives

```c
typedef struct {
    void (*WdtFeed)(void);     /* feed the watchdog before reset */
    void (*SystemReset)(void); /* fallback reset; does not return */
} IOtaEntrySys_t;
```

Used by `ota_entry_core`:

- `OtaEntry_EnterBootloader(sys)` — set the bootloader command in `FW_Info` and
  jump back to the bootloader (with vector relocation). Does not return.
- `OtaEntry_ConfirmHealth(sys)` — mark the running bank `CONFIRMED` and clear the
  trial counter so the bootloader won't roll back. Idempotent.
- `OtaEntry_ValidateFirmware(sys)` — boot-time gate: CRC + bank-usage check, promote
  `VALID → ACTIVE`, fail → enter bootloader (rollback). Fresh flash passes through.

Precondition: `FlashService_Init(driver)` must have run.

---

## 5. `IOtaFwdTx_t` + profile — chain nodes only

Transport vtable (one per node); semantics come from a built-in profile — **don't
write your own FSM**.

```c
typedef struct {
    _Bool (*SendPacket)(uint8_t *frame_buf, uint16_t frame_len, uint8_t child_id);
    void  (*Settle)(void);     /* soft flush after one child completes */
    void  (*HardFlush)(void);  /* hard flush at session end */
} IOtaFwdTx_t;
```

- `SendPacket`: `buf[2]=cmd`, `buf[3..]` already filled by the FSM; your adapter
  frames it (header / id / checksum / terminator) and enqueues. Return `1` on
  success, `0` if the TX queue is full (retried next tick).
- Pick a profile (`extern const`): `OTA_FWD_PROXY_PROFILE` (downstream is a
  bootloader, ENTER handshake, 20-byte `UPDATE_META`) or `OTA_FWD_RELAY_PROFILE`
  (downstream is an app relay, 21-byte `UPDATE_META` with node-id prefix).
- Context is caller-held: `OtaForwarder_Init(&ctx, &sys, &tx, profile, child_max, frame_len)`.
- Feed child ACKs via `OtaForwarder_OnChildAck(&ctx, child_id, ota_cmd)`, tick with
  `OtaForwarder_Task(&ctx)`.
