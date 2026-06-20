[English](injection-points.md) | **繁體中文**

# 注入點

每個平台相依都是一個你填寫並注入的 vtable。核心持有純 C 狀態機,硬體差異藏在函式指標後面。
**回傳碼約定:`int32_t` 一律 `0` = OK、負 = 錯。**

---

## 1. `IFmcDriver_t` — Flash 驅動  ★最關鍵

定義在 `ota_flash_port.h`。唯一真正碰暫存器的一層——**先單獨單測**
(erase 一頁 → write words → read 回來比對 → 檢查 `GetCRC32`)。

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

- **單位**:`WriteWords` / `ReadWords` 吃的是 **word(4-byte)數**,不是 byte 數。
- **對齊**:`ErasePage(addr)` 必須 page 對齊;`WriteWords` 起點通常要 word/雙 word 對齊(看 MCU)。
- `GetActiveBank()`:目前**執行中**的是哪個 bank——Cortex-M0 用 vendor VECMAP,
  M0+/M3/M4 用 VTOR 或自存旗標。fresh flash 要回安全預設(Bank 0)。
- `JumpToApp(addr)`:設好向量表重定位再跳;**不返回**。

主迴圈前以 `FlashService_Init(&driver)` 注入。
參考 mock:[`examples/host_sim/mock_flash.c`](../examples/host_sim/mock_flash.c)。

---

## 2. `IOtaSys_t` — 毫秒計數

```c
typedef struct {
    uint32_t (*GetTickMs)(void);   /* 單調遞增毫秒;32-bit 溢位以無號差值處理 */
} IOtaSys_t;
```

所有逾時用 `(int32_t)(now - deadline) >= 0`,故計數器 wrap 不需特別處理。
接你的 SysTick / 1ms 計數器即可。

---

## 3. `IOtaNet_t` — 收 / 送(`ota_scheduler` 用)

```c
typedef struct {
    int32_t (*SendUpstream)(uint8_t cmd, const uint8_t *payload, uint16_t len);
    int32_t (*SendDownstream)(uint8_t node_id, uint8_t cmd, const uint8_t *payload, uint16_t len);
    void    (*TriggerProxy)(uint8_t bank_index, uint32_t payload_size, uint32_t transport_crc32,
                            uint32_t fw_image_size, uint32_t version, uint8_t target_node_id);
} IOtaNet_t;
```

- `SendUpstream`:往來源回 ACK / 狀態(葉節點主要用這個)。
- `SendDownstream`:只有鏈式節點用;葉節點可留 no-op。
- `TriggerProxy`:scheduler 在整支映像存完**且** CRC 過後呼叫。**葉節點**→ 跳進 Bootloader
  (忽略 `target_node_id`);**鏈式節點**→ 委派給 `OtaForwarder_Trigger`。
- 你的傳輸 ISR 解出一個 frame 後呼叫 `OtaScheduler_OnReceivePacket(cmd, payload, len)`——
  其中 **`len` 必須等於實際 chunk 長度**(≤ `OTA_CHUNK_SIZE`);見踩雷文件。

以 `OtaScheduler_Init(role, &net, &sys)` 注入。

---

## 4. `IOtaEntrySys_t` — Bootloader 入口原語

```c
typedef struct {
    void (*WdtFeed)(void);     /* reset 前餵狗 */
    void (*SystemReset)(void); /* 後備重置;不返回 */
} IOtaEntrySys_t;
```

`ota_entry_core` 使用:

- `OtaEntry_EnterBootloader(sys)` — 把 Bootloader 命令寫進 `FW_Info`,跳回 Bootloader
  (含向量重定位)。不返回。
- `OtaEntry_ConfirmHealth(sys)` — 把執行中 bank 標 `CONFIRMED`、清試啟動計數,防 Bootloader
  rollback。冪等。
- `OtaEntry_ValidateFirmware(sys)` — 開機閘:CRC + bank usage 檢查,`VALID → ACTIVE` 升級,
  失敗 → 進 Bootloader(rollback)。fresh flash 直接放行。

前提:`FlashService_Init(driver)` 必須已執行。

---

## 5. `IOtaFwdTx_t` + profile — 僅鏈式節點

傳輸 vtable(每節點一份);語意走內建 profile——**不要自己寫 FSM**。

```c
typedef struct {
    _Bool (*SendPacket)(uint8_t *frame_buf, uint16_t frame_len, uint8_t child_id);
    void  (*Settle)(void);     /* 單台完成後軟 flush */
    void  (*HardFlush)(void);  /* session 結束硬 flush */
} IOtaFwdTx_t;
```

- `SendPacket`:`buf[2]=cmd`、`buf[3..]` 已由 FSM 填好;你的 adapter 負責建框
  (header / id / checksum / 終止)並入隊。成功回 `1`,TxQ 滿回 `0`(下個 tick 重試)。
- 選 profile(`extern const`):`OTA_FWD_PROXY_PROFILE`(下游是 Bootloader,ENTER 握手、
  20-byte `UPDATE_META`)或 `OTA_FWD_RELAY_PROFILE`(下游是 App 中繼,21-byte `UPDATE_META`
  含 node-id 前綴)。
- context 由 caller 持有:`OtaForwarder_Init(&ctx, &sys, &tx, profile, child_max, frame_len)`。
- 子節點 ACK 進來時呼 `OtaForwarder_OnChildAck(&ctx, child_id, ota_cmd)`,每 tick 呼
  `OtaForwarder_Task(&ctx)`。
