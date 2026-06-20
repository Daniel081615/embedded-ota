**English** | [繁體中文](architecture.zh-TW.md)

# Architecture

`embedded-ota` is the **App-side portable core** of an A/B firmware-update system:
it receives an image, stages it into the inactive flash bank, verifies it, and on
trigger either jumps to a bootloader (leaf) or relays it downstream (chain node).
The bootloader that performs the actual swap-on-boot is platform-specific and not
part of this repo; the two sides share only `fw_info.h`.

## Store-and-Trigger model

The receive path is deliberately split into two phases so the core stays simple
and the (optional, complex) forwarding logic is fully decoupled:

1. **Store** — `ota_scheduler` receives the image chunk-by-chunk, writes each
   chunk to the inactive bank through the injected flash driver, and verifies the
   transport CRC32 locally. It does *not* know how to forward.
2. **Trigger** — once stored and verified, a separate `TRIGGER` command invokes
   the injected `TriggerProxy` callback, which decides what happens next:
   - **leaf node** → jump to the bootloader (which flashes on next boot),
   - **chain node** → hand off to `ota_forwarder` to burn downstream children.

This keeps `ota_scheduler` identical for every role; only the injected
`TriggerProxy` differs.

## Module responsibilities

| File | Layer | Responsibility | State machine |
|---|---|---|---|
| `ota_flash_port.h` | contract | flash driver vtable `IFmcDriver_t` + geometry constants (`OTA_FLASH_PAGE_SIZE` / `BANK_SIZE`, `-D`-overridable) | — |
| `fw_info.h` | contract | on-flash `FW_Info_t` (8B) / `Bank_MetaInfo_t` (16B) layout + all enums + `_Static_assert` layout guards | — |
| `ota_protocol.h` | contract | wire encoders: `UPDATE_META` (20B), chunk offset (4B). Pure byte-packing, no I/O | — |
| `flash_service.{c,h}` | service | bank / meta / firmware staging through the injected `IFmcDriver_t`; never touches registers | — |
| `ota_scheduler.{c,h}` | receive FSM | store-and-trigger receiver, parameterized by `OtaRole_t` | yes |
| `ota_forwarder.{c,h}` | forward FSM | "upstream burns downstream children" async state machine; transport vs semantics split into two orthogonal vtables | yes |
| `ota_entry_core.{c,h}` | entry | enter-bootloader / confirm-health / boot-time integrity validate; flash via `flash_service`, platform primitives via `IOtaEntrySys_t` | — |

## Dependency graph

The core has **zero external includes** — only intra-repo headers plus
`<stdint.h>` / `<string.h>` / `<stddef.h>`:

```
fw_info.h        ← base contract, no dependencies
ota_protocol.h   ← standalone (stdint only)
ota_flash_port.h ← standalone (stdint only); provides IFmcDriver_t + geometry

flash_service.h  → fw_info.h, ota_flash_port.h
ota_scheduler.h  → fw_info.h
ota_forwarder.h  → ota_scheduler.h  (IOtaSys_t / OTA_CMD_* / OTA_CHUNK_SIZE)
ota_entry_core.* → flash_service    (requires FlashService_Init() first)
```

## Forwarding: one FSM, two vtables

For chain topologies, `ota_forwarder` runs a single state machine and pushes all
differences into two orthogonal interfaces:

- `IOtaFwdTx_t` — **transport** primitives (how to put a frame on the wire / flush).
- `IOtaFwdProfile_t` — **semantics** (handshake or not, how `UPDATE_META` is
  encoded, what to do on stream-complete / child-fail).

Two profiles ship built-in: `OTA_FWD_PROXY_PROFILE` (downstream is a bootloader,
needs an ENTER handshake) and `OTA_FWD_RELAY_PROFILE` (downstream is an app relay).
A leaf node doesn't compile this module at all.

## Design rules (kept while editing)

- **No blocking in the core.** "Wait for an event" is expressed as a state, not a
  spin loop. Short hardware waits live inside the injected driver only.
- **Actor shape.** Forwarder context is caller-held (`OtaFwdCtx_t`), no singletons
  that assume one caller — ready to become RTOS task-private data.
- **ISR-safe by construction.** ISRs only fill buffers / set flags; the FSMs
  advance in the main loop via `*_Task()`.
- **No `malloc`, no recursion, no float.** Page read-modify-write uses a static
  buffer, never the stack.
