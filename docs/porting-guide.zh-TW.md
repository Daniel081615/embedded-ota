[English](porting-guide.md) | **繁體中文**

# 移植指南

把 `embedded-ota` 帶到新的 MCU / 傳輸 / 拓樸。大部分工作是實作幾個 vtable
([injection-points.zh-TW.md](injection-points.zh-TW.md));本文講決策與陷阱。

## 決策樹(動工前先回答)

1. **拓樸** — 單節點直收主機,還是鏈式逐台燒下游?
   - 不轉發 → **不要編譯** `ota_forwarder.{c,h}`;`TriggerProxy` 直接進 Bootloader。省掉最複雜的一塊。
   - 轉發 → 選 `OTA_FWD_PROXY_PROFILE`(下游是 Bootloader)或 `OTA_FWD_RELAY_PROFILE`(下游是 App 中繼)。
2. **Flash 預算** — 容得下兩個完整 bank(A/B)嗎?
   - 容得下 → 直接用雙 bank 的 `flash_service`。
   - 容不下 → 單 bank + Bootloader staging;`flash_service.c` 的 bank index 語意與位址常數要重做。
3. **核心** — Cortex-M0(vendor VECMAP)還是 M0+/M3/M4(VTOR)?決定 `JumpToApp` / `GetActiveBank`
   怎麼重定位與回報向量表。
4. **傳輸** — 沿用 100-byte UART frame,還是別的(TCP/BLE/CAN/USB)?
   - 換傳輸 → 重做 `IOtaNet_t`(+ `IOtaFwdTx_t`);**FSM 不變**。重算 `OTA_CHUNK_SIZE` 與框長。
5. **工具鏈** — 要過不只一個編譯器嗎?核心刻意只用純 C11、無編譯器擴充;請維持這點以保可攜。

## 可調預設 vs 固定契約

MCU/傳輸 相依、可改的預設值:

- `ota_flash_port.h` 的 `OTA_FLASH_PAGE_SIZE` / `OTA_FLASH_BANK_SIZE` —— NUC1261 值,
  以 `-DOTA_FLASH_PAGE_SIZE=...` / `-DOTA_FLASH_BANK_SIZE=...` 覆寫。
- bank / meta / FW_Info **位址** —— `flash_service.c` 的 `FS_*` `#ifndef`,以 `-D` 覆寫(不改源碼)。
- `OTA_CHUNK_SIZE`(`ota_scheduler.h`)—— 綁你的框大小。
- `OtaCommand_t` 的值與 100-byte frame 是**參考**協定;兩端對齊即可。

固定契約(改 = 全體都改):`fw_info.h` struct 佈局、`ota_protocol.h` 的 `UPDATE_META` / chunk 編碼。

## 帶起順序(由下而上,逐層驗)

1. **Flash 驅動先做** — 填 `IFmcDriver_t`,單測 erase/write/read/CRC。這是最容易做錯、
   也最難 debug 的一層(flash 靜默損壞)。
2. **佈局** — 上目標平台 build,確認 `_Static_assert` 過(需 `-fshort-enums`)。
3. **葉節點接收** — 填 `IOtaSys_t` + `IOtaNet_t`,跑 `ota_scheduler` 直到一支映像存完且 CRC 驗過。
   (host sim 就是這層的範本。)
4. **進 BL / 確認健康** — 填 `IOtaEntrySys_t`,接 `ota_entry_core`。
5. **(可選)轉發** — 填 `IOtaFwdTx_t` + 選 profile。

## 踩雷清單(量產學到的)

- **chunk 長度必須等於 `OTA_CHUNK_SIZE`。** dispatcher 必須把**實際 chunk payload 長度**
  交給 scheduler。傳一個較大的 host 層長度,會讓 `len > OTA_CHUNK_SIZE` 的守門整包丟掉
  → 更新悄悄不前進。
- **逐包閒置逾時,不是全域逾時。** `OTA_RECV_TIMEOUT_MS` 每收一包就重置。單一全域 deadline
  會讓大映像傳到一半逾時(經典的「卡在 ~78%」)。
- **bank 大小要對齊**,讓 `EraseBank` 永不越界抹到下一區。頁讀改寫用 **static** buffer——
  別搬到 stack(2KB 頁會爆小 stack → HardFault)。
- **`_Static_assert` 必須在目標平台過。** 沒過代表 enum 不是 1 byte——修編譯器旗標,
  絕不要 `#if 0` 掉守門。它是唯一阻止 on-flash 佈局靜默損壞的東西。
- **別讓輪詢把觸發短路掉。** 存完後,`TriggerProxy → 轉發 / 進 BL` 這條路徑必須在主迴圈裡
  跑得到,不被其他輪詢條件擋掉。
- **無 `malloc` / 遞迴 / 浮點。** ISR 保持短小(填 buffer / 設旗標);FSM 在主迴圈推進。
