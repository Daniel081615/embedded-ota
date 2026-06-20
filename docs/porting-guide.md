# Porting guide

Bringing `embedded-ota` to a new MCU / transport / topology. Most of the work is
implementing a few vtables ([injection-points.md](injection-points.md)); this doc
covers the decisions and the traps.

## Decision tree (answer before you start)

1. **Topology** — single node receiving from a host, or a chain that burns
   downstream children?
   - No forwarding → **don't compile** `ota_forwarder.{c,h}`; `TriggerProxy` just
     enters the bootloader. Saves the most complex piece.
   - Forwarding → choose `OTA_FWD_PROXY_PROFILE` (downstream is a bootloader) or
     `OTA_FWD_RELAY_PROFILE` (downstream is an app relay).
2. **Flash budget** — room for two full banks (A/B)?
   - Yes → use the dual-bank `flash_service` as-is.
   - No → single-bank + bootloader staging; the bank-index semantics and address
     constants in `flash_service.c` need rework.
3. **Core** — Cortex-M0 (vendor VECMAP) or M0+/M3/M4 (VTOR)? Decides how
   `JumpToApp` / `GetActiveBank` relocate and report the vector table.
4. **Transport** — reuse the 100-byte UART frame, or something else (TCP/BLE/CAN/USB)?
   - Different transport → reimplement `IOtaNet_t` (+ `IOtaFwdTx_t`); **the FSMs
     don't change**. Recompute `OTA_CHUNK_SIZE` and the frame length.
5. **Toolchains** — need to pass more than one compiler? The core is deliberately
   plain C11 with no compiler extensions; keep it that way to stay portable.

## What's a configurable default vs a fixed contract

Defaults that are MCU/transport-specific and meant to be changed:

- `OTA_FLASH_PAGE_SIZE` / `OTA_FLASH_BANK_SIZE` in `ota_flash_port.h` — NUC1261
  values; override with `-DOTA_FLASH_PAGE_SIZE=...` / `-DOTA_FLASH_BANK_SIZE=...`.
- Bank / meta / FW_Info **addresses** — `FS_*` `#ifndef` in `flash_service.c`,
  override with `-D` (no source edit).
- `OTA_CHUNK_SIZE` (`ota_scheduler.h`) — tied to your frame size.
- The `OtaCommand_t` values and the 100-byte frame are the *reference* protocol;
  both ends must simply agree.

Fixed contracts (change = change everyone): `fw_info.h` struct layout, the
`UPDATE_META` / chunk encodings in `ota_protocol.h`.

## Bring-up order (bottom-up, verify each layer)

1. **Flash driver first** — fill `IFmcDriver_t`, unit-test erase/write/read/CRC.
   This is the easiest layer to get wrong and the hardest to debug (silent flash
   corruption).
2. **Layout** — build on target, confirm `_Static_assert` passes (needs `-fshort-enums`).
3. **Leaf receive** — fill `IOtaSys_t` + `IOtaNet_t`, run `ota_scheduler` until a
   full image stores and CRC verifies. (The host sim is a template for this.)
4. **Enter BL / confirm health** — fill `IOtaEntrySys_t`, wire `ota_entry_core`.
5. **(Optional) forwarding** — fill `IOtaFwdTx_t` + pick a profile.

## Pitfalls (learned in production)

- **Chunk length must equal `OTA_CHUNK_SIZE`.** The dispatcher must hand the
  scheduler the *actual chunk payload length*. Passing a larger host-layer length
  makes the `len > OTA_CHUNK_SIZE` guard drop whole chunks → the update silently
  fails to progress.
- **Per-chunk idle timeout, not a global one.** `OTA_RECV_TIMEOUT_MS` is reset on
  every chunk. A single global deadline makes large images time out mid-transfer
  (the classic "stuck at ~78%").
- **Bank size must be aligned** so `EraseBank` never erases into the next region.
  Page read-modify-write uses a **static** buffer — don't move it to the stack
  (2 KB pages overflow small stacks → HardFault).
- **`_Static_assert` must pass on target.** If it fails, you don't have 1-byte
  enums — fix the compiler flag, never `#if 0` the guard. It's the only thing
  stopping a silent on-flash layout corruption.
- **Don't let polling short-circuit the trigger.** After store completes, the
  `TriggerProxy → forward / enter-BL` path must run in the main loop regardless of
  other poll gates.
- **No `malloc` / recursion / float.** Keep ISRs short (fill buffer / set flag);
  advance the FSMs in the main loop.
