[English](architecture.md) | **繁體中文**

# 架構

`embedded-ota` 是 A/B 韌體更新系統的 **App 側可攜核心**:接收映像、存進未啟用的 flash bank、
驗證,並在觸發時依角色跳進 Bootloader(葉節點)或轉發給下游(鏈式節點)。真正在下次開機
做切換的 Bootloader 是平台相依的、不在本 repo;兩側只共用 `fw_info.h`。

## Store-and-Trigger 模型

接收路徑刻意拆成兩個階段,讓核心保持簡單,並把(可選、複雜的)轉發邏輯完全解耦:

1. **Store(接收期)** — `ota_scheduler` 逐包接收映像,透過注入的 flash 驅動把每包寫進
   未啟用的 bank,並在本地驗證 transport CRC32。它**不知道**如何轉發。
2. **Trigger(觸發期)** — 存完並驗過後,一個獨立的 `TRIGGER` 指令呼叫注入的
   `TriggerProxy` 回呼,由它決定接下來:
   - **葉節點** → 跳進 Bootloader(下次開機燒錄),
   - **鏈式節點** → 交給 `ota_forwarder` 逐台燒下游子節點。

如此 `ota_scheduler` 對每種角色都完全相同,只有注入的 `TriggerProxy` 不同。

## 模組職責

| 檔 | 層 | 職責 | 狀態機 |
|---|---|---|---|
| `ota_flash_port.h` | 契約 | flash 驅動 vtable `IFmcDriver_t` + 幾何常數(`OTA_FLASH_PAGE_SIZE` / `BANK_SIZE`,可 `-D` 覆寫) | — |
| `fw_info.h` | 契約 | on-flash `FW_Info_t`(8B)/ `Bank_MetaInfo_t`(16B)佈局 + 所有 enum + `_Static_assert` 守佈局 | — |
| `ota_protocol.h` | 契約 | wire 編碼器:`UPDATE_META`(20B)、chunk offset(4B)。純 byte-packing,無 I/O | — |
| `flash_service.{c,h}` | 服務 | 透過注入的 `IFmcDriver_t` 做 bank / meta / 韌體 staging;永不碰暫存器 | — |
| `ota_scheduler.{c,h}` | 接收 FSM | store-and-trigger 接收器,以 `OtaRole_t` 參數化 | 有 |
| `ota_forwarder.{c,h}` | 轉發 FSM | 「上游逐台燒下游」非同步狀態機;傳輸與語意拆進兩個正交 vtable | 有 |
| `ota_entry_core.{c,h}` | 入口 | 進 Bootloader / 確認健康 / 開機完整性驗證;flash 走 `flash_service`,平台原語走 `IOtaEntrySys_t` | — |

## 相依圖

核心**零外部 include**——只有 repo 內部 header 加 `<stdint.h>` / `<string.h>` / `<stddef.h>`:

```
fw_info.h        ← 底層契約,無相依
ota_protocol.h   ← 獨立(只 stdint)
ota_flash_port.h ← 獨立(只 stdint);提供 IFmcDriver_t + 幾何常數

flash_service.h  → fw_info.h, ota_flash_port.h
ota_scheduler.h  → fw_info.h
ota_forwarder.h  → ota_scheduler.h  (IOtaSys_t / OTA_CMD_* / OTA_CHUNK_SIZE)
ota_entry_core.* → flash_service    (需先 FlashService_Init())
```

## 轉發:一個 FSM、兩個 vtable

鏈式拓樸下,`ota_forwarder` 跑單一狀態機,把所有差異推進兩個正交介面:

- `IOtaFwdTx_t` — **傳輸**原語(怎麼把一個 frame 送上線 / flush)。
- `IOtaFwdProfile_t` — **語意**(要不要握手、`UPDATE_META` 怎麼編、stream 完成 / 子節點失敗時做什麼)。

內建兩個 profile:`OTA_FWD_PROXY_PROFILE`(下游是 Bootloader,需 ENTER 握手)與
`OTA_FWD_RELAY_PROFILE`(下游是 App 中繼)。葉節點根本不編譯這個模組。

## 設計鐵則(撰寫時遵守)

- **核心不阻塞。**「等事件」用狀態表達,不用 spin loop。短的硬體等待只活在注入的驅動裡。
- **Actor 形狀。** forwarder 的 context 由 caller 持有(`OtaFwdCtx_t`),沒有假設單一呼叫者的
  單例——日後可直接變成 RTOS task-private 資料。
- **天生 ISR-safe。** ISR 只填 buffer / 設旗標;FSM 在主迴圈透過 `*_Task()` 推進。
- **無 `malloc`、無遞迴、無浮點。** 頁讀改寫用 static buffer,絕不用 stack。
