# embedded-ota

[![ci](https://github.com/Daniel081615/embedded-ota/actions/workflows/ci.yml/badge.svg)](https://github.com/Daniel081615/embedded-ota/actions/workflows/ci.yml)
![license](https://img.shields.io/badge/license-MIT-blue)
![lang](https://img.shields.io/badge/C-C11-informational)

[English](README.md) | **繁體中文**

一套可攜、OS 無關的 **雙 bank、store-and-trigger OTA 核心**,給裸機 MCU 韌體用。
純 C11、無動態記憶體、無浮點、無遞迴——硬體全部抽象在注入的函式指標 vtable 之後,
所以同一份源碼能跑在任何 MCU 上(也能在 PC 上跑,見 [`examples/host_sim`](examples/host_sim))。

它負責韌體更新的 **接收 / 驗證 / 轉發** 這一側:一個節點透過任意傳輸層接收映像,
存進未啟用的 flash bank、驗證 CRC,並在觸發時依角色決定——葉節點跳進 Bootloader、
鏈式節點則把映像逐台轉發給下游子節點。真正在下次開機做 A/B 切換的 Bootloader
是平台相依的、不在本 repo 範圍;兩側只透過共用的 `fw_info.h` flash 佈局對齊。

> 抽取自一套量產韌體 mono-repo(NDHU 宿舍智慧電錶系統:Gateway → Concentrator → RoomNode
> 三層鏈,Nuvoton NUC1261 / Cortex-M0,以 Keil armclang 建置並用 arm-gcc 交叉驗證)。
> 本 repo 只含可攜核心——不含任何應用 / 業務程式碼。

## 為什麼可攜

所有平台相依都以 vtable 注入。核心永不 include MCU header、永不碰暫存器、永不阻塞、
也永不直接呼叫 OS 原語:

| 你要實作的 | 介面 | 用途 |
|---|---|---|
| Flash 驅動 | `IFmcDriver_t` | erase / write / read / CRC / 跳轉 / 取執行中 bank |
| 毫秒計數 | `IOtaSys_t` | 逾時判斷(無號差值,溢位安全) |
| 傳輸收/送 | `IOtaNet_t` | 上行/下行送包、觸發轉發或開機 |
| 餵狗 / 重置 | `IOtaEntrySys_t` | 進 Bootloader、確認健康、開機驗證 |
| (僅鏈式)框傳輸 | `IOtaFwdTx_t` + profile | 驅動下游子節點 |

逐欄位契約見 [docs/injection-points.zh-TW.md](docs/injection-points.zh-TW.md)。

## 快速開始(在你的 PC 上跑)

```sh
make -C examples/host_sim run
```

這會注入以 RAM 模擬的 mock 驅動,帶一個葉節點走完整個更新流程——
`UPDATE → STORE×N → CRC 驗證 → TRIGGER`——證明核心零硬體即可編譯運行:

```
== embedded-ota host_sim ==
firmware: 256 bytes, crc=0xD48DDFE9
-> UPDATE_CHILD_REQ
-> STORE chunks
-> TRIGGER_CHILD_REQ
    TriggerProxy: bank=1 payload=256 crc=0xD48DDFE9 ver=1  (leaf -> would enter bootloader)
PASS: 256-byte firmware stored, CRC verified, trigger fired.
```

### ⚠️ 建置要求:`-fshort-enums`

`fw_info.h` 以 `_Static_assert(sizeof(FW_Info_t)==8 ...)` 守住 flash 佈局,
而這只有在 enum 為 1 byte 時才成立。**所有消費端都必須以 `-fshort-enums` 編譯**
(armclang 預設即為 short)。沒有它,build 會卡在 static assert 而失敗——這是刻意設計,
讓「佈局不符」永遠不會在實機上靜默寫壞 flash。

## 檔案

```
ota_flash_port.h   flash 驅動 vtable(IFmcDriver_t)+ 幾何常數
fw_info.h          on-flash FW_Info / Bank_MetaInfo 佈局 + enum(與 Bootloader 共用)
ota_protocol.h     wire 編碼器(UPDATE_META 20B、chunk offset 4B)
flash_service.{c,h}  透過注入的 flash 驅動做 bank / meta / 韌體 staging
ota_scheduler.{c,h}  接收 FSM(store-and-trigger),以角色參數化
ota_forwarder.{c,h}  轉發 FSM(上游逐台燒下游)——可選用
ota_entry_core.{c,h} 進 Bootloader / 確認健康 / 開機驗證
```

## 文件

- [docs/architecture.zh-TW.md](docs/architecture.zh-TW.md) — 模組職責、相依圖、設計鐵則
- [docs/injection-points.zh-TW.md](docs/injection-points.zh-TW.md) — 五個 vtable,逐欄位
- [docs/wire-protocol.zh-TW.md](docs/wire-protocol.zh-TW.md) — OTA 指令、UPDATE_META / chunk 編碼、fw_info 佈局
- [docs/porting-guide.zh-TW.md](docs/porting-guide.zh-TW.md) — 決策樹、要改什麼、踩雷清單

## 限制

純 C11 · 無 `malloc`/`free` · 無遞迴 · 無浮點 · ISR 只設旗標 / 填 buffer(FSM 在主迴圈推進)。
設計上保持可攜到 RTOS:每個模組都是 actor、context 由 caller 持有、不依賴呼叫順序、
OS 原語只透過注入介面取用。

## 授權

MIT — 見 [LICENSE](LICENSE)。
