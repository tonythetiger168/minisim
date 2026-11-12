# GitHub／OpenCores 代表性 IP 專案與 OpenTitan

2026-11-01。使用者已選擇順序：**先完成兩個 portfolio，再跑代表性外部 IP**；另明確指定 OpenTitan。下面是已核對存在的專案與固定 revision，不代表已全部完成測試。

| 優先 | 專案 | 本輪鎖定 HEAD | 用途／初始測試範圍 |
|---|---|---|---|
| 1 | [lowRISC/OpenTitan](https://github.com/lowRISC/opentitan) | 41049e9deb40e0602cd5272c40398be380ae2637 | 已 clone 到 workspace/opentitan；先盤點 GPIO／UART／SPI host／I2C／AES／HMAC 的上游開源 lint 與測試入口 |
| 2 | [freecores/uart16550](https://github.com/freecores/uart16550) | 2b0ad80d0968f201f95506854ad769c8a44abe2b | UART 16550，優先 upstream testbench、Wishbone register 與串列交易 |
| 2 | [freecores/i2c](https://github.com/freecores/i2c) | 3b067f00ccced753b0502024766a51f58f3e04bc | I2C controller，優先 upstream bench 與 open-drain 行為 |
| 2 | [freecores/simple_spi](https://github.com/freecores/simple_spi) | 61c39a5f79fd8477d78f7b04edf6d9d530a904af | SPI core，優先 upstream bench 與 clock/data timing |
| 2 | [freecores/gpio](https://github.com/freecores/gpio) | 73eafb007e6718b1503d17ae3e0c06139109d345 | GPIO core，優先 register／direction／interrupt；無 bench 時標示缺測，不能用編譯代替動態通過 |

FreeCores 是這批 OpenCores IP 的 GitHub mirror；同一專案不能在 GitHub/OpenCores 各算一次獨立樣本。GitHub API 未自動分類這四個 repo 的 license，後續必須讀原始 header／license 文件，保留歸屬；不擅自視作 MIT/Apache。OpenTitan README 指出大部分內容為 Apache-2.0，個別檔案依標註。

## OpenTitan 已完成盤點

- repo checkout 在 mainline master，HEAD 如上，工作目錄乾淨；不是使用另一個同名非官方鏡像。
- `.bazelversion` 為 8.7.0；不能假定系統預裝的其他 Bazel 版本可替代。
- UART core：`hw/ip/uart/uart.core`，DV target 在 `hw/ip/uart/dv/uart_sim.core`。
- SPI host／I2C／AES／HMAC：`hw/ip/spi_host/`、`hw/ip/i2c/`、`hw/ip/aes/`、`hw/ip/hmac/`。
- GPIO 為 top-specific 產物；初始選 `hw/top_earlgrey/ip_autogen/gpio/gpio.core`，不錯用不存在的舊 `hw/ip/gpio` 路徑。
- AES 等 core 含 Verilator lint target；UART DV sim 的預設工具是 VCS。讀到 target 不等於已完成執行；商用 DV 不能自行標成開源模擬通過。
- 首批先建立實際依賴／fileset／FuseSoC/DVSim 入口，再跑可執行的開源 checks；每項註明 lint、elaboration、smoke、self-checking sim 或完整 DV，不混為一個 PASS。

## 接續動作

1. 完成 portfolio 的 129 IP 與 130 VIP 回歸／coverage／UVM；先處理 timeout、環境失效與未支援分類。
2. 準備 OpenTitan 選定 IP 的依賴與測試 manifest，逐一跑最小 upstream 開源檢查；完整 SoC／所有 UVM tests 未涵蓋時明說。
3. 再 checkout 四個 FreeCores 專案的固定 revision、保留 license，依各自 upstream bench 執行一次。
4. 最後做 Minisim compatibility 與差異驗證；不支援的 parameter、hierarchy、async reset、UVM、SVA 等必須列為 blocker，不能把參考模擬器的 PASS 當作 Minisim PASS。

仍未宣稱已掃遍 GitHub/OpenCores 或已完成 OpenTitan SoC 驗證。所有後續沿用每小時跟進，不推送、合併或啟動付費工具。


## 2026-11-03：OpenTitan／OpenCores 可追溯準備

新增 `EXTERNAL_IP_INTAKE.json`：OpenTitan 六個 core 共72個 literal HDL來源項目存在且保存SHA256；這是檔案盤點，不是FuseSoC依賴解析或動態模擬。四個FreeCores固定revision已核對上游testbench，包含UART16550、I2C、Simple SPI與GPIO；GPIO已有bench，不再視為缺測。FreeCores檔案授權按header註記，未外推為全repo授權。

核對日期為2026-11-03，非上述專案發布日期；未將舊版本盤點當24小時新發布。來源與固定版本在manifest的header_source及本頁表格。順序仍為先完成兩個portfolio，再執行上述外部IP，最後做Minisim相容性／差異驗證。每30分鐘跟進。本輪外部IP模擬0次，未宣稱PASS。

背景狀態：Docker可查詢，ai-eda-portfolio-20261101為exited；未重新啟動大編譯。下一步先核對資源、既有checkpoint與未完成VIP coverage，再使用既有resume入口續跑；不重跑已完成階段。
