# FPGA 工具與最小資源／bitstream 工作範圍

2026-11-01 新增使用者需求。延續兩個 portfolio、OpenTitan 與代表性 IP 的順序；FPGA 工具先做小型離線基線，不增加大型平行編譯。

## GitHub 一手來源

已經 GitHub API 核對下列 12 個官方 repo，revision、commit 日期與 API license 分類保存在 [fpga-tools-lock.json](fpga-tools-lock.json)。這是採樣日期的基線，不把舊專案當作今日新發布；commit 日期亦不是 release 日期。NOASSERTION 項須另外核對檔案授權。

| 優先 | 專案 | 範圍與用途 |
|---|---|---|
| P0 | [Yosys](https://github.com/YosysHQ/yosys) | FPGA 綜合與 mapping，掃描可重現設定 |
| P0 | [nextpnr](https://github.com/YosysHQ/nextpnr) | P&R、資源與時序報告；固定 device/package/seed |
| P0 | [IceStorm](https://github.com/YosysHQ/icestorm) | iCE40 pack/unpack 與 bitstream 大小 |
| P1 | [Project Trellis](https://github.com/YosysHQ/prjtrellis) | ECP5 資料與 bitstream 路線 |
| P1 | [Apicula](https://github.com/YosysHQ/apicula) | Gowin 資料與 bitstream 路線 |
| P1 | [OSS CAD Suite](https://github.com/YosysHQ/oss-cad-suite-build) | 版本化工具發行；build repo 授權不代表所有套件授權 |
| P1 | [FuseSoC](https://github.com/olofk/fusesoc) | IP fileset 與相依管理，對接 OpenTitan |
| P1 | [Edalize](https://github.com/olofk/edalize) | 可重用 backend 建置介面 |
| P2 | [openFPGALoader](https://github.com/trabucayre/openFPGALoader) | 後續板卡下載；目前只規劃，不自動寫板或 flash |
| P2 | [F4PGA](https://github.com/f4pga/f4pga) | 工具鏈文件與入口；鎖定 commit 是 2023 年，非新發布 |
| P2 | [Project X-Ray](https://github.com/f4pga/prjxray) | Xilinx 7-series bitstream 研究，需逐器件確認 |
| P2 | [LiteX](https://github.com/enjoy-digital/litex) | SoC／板卡整合案例，不能當成 P&R 工具 |

## 最佳化驗收契約

同一功能、介面、位寬、reset、吞吐與延遲下比較；時序必須達標，不能靠刪除必要功能降低資源。保留 LUT、FF、BRAM、DSP、P&R logic cells 各項數值，不用任意加總掩蓋 tradeoff。候選可掃 mapping、FSM encoding、resource sharing、memory inference、合法位寬、placement seed；RTL 改寫需另做等價驗證。

bitstream 分別記錄可直接配置的原始檔、器件支援的壓縮格式、傳輸用壓縮檔；gzip 變小不能當作硬體配置 bitstream 變小。較少 LUT 不保證較小 bitstream；固定器件配置框架可能保持大小。較小器件另成比較組，須滿足 IO、記憶體與時序限制。只稱「已測候選中最佳」，不保證全域最小。

## 已執行基線

WSL：Yosys 0.52、nextpnr-ice40 0.9-3、icepack/iceunpack。ECP5 工具、openFPGALoader、FuseSoC 尚未在此環境發現；沒有宣稱全部安裝。

重跑：`python tools/run_fpga_baseline.py`，需上述工具與 Icarus、`/usr/share/yosys/ice40/cells_sim.v`。結果在 `build/fpga-baseline/report.json`，另保存來源 SHA、版本、逐階段 log、netlist、ASC、binary、資源與時序報告。

| 設定 | P&R logic cells | 原始 bitstream bytes | gate/reference samples |
|---|---:|---:|---:|
| default | 11 | 32220 | 300 pass |
| abc2 | 11 | 32220 | 300 pass |
| nocarry | 12 | 32220 | 300 pass |

HX1K TQ144、12 MHz、seed 1，三者時序達標，pack→unpack→repack 位元組一致。這是 8-bit counter 的 900 筆二態 gate-level 驗證，不是 formal equivalence、Minisim backend 驗收或板卡測試。使用自動 IO placement，不能直接當作實際板卡 pinout。default 與 abc2 是此候選集合的 Pareto 最佳，未改善 baseline 檔案大小。

## 待辦與跨 repo 接口

1. P0：將此報告契約接到 Helios 的可行候選評估，保留功能證據、約束、工具版本、raw metrics、fail/timeout；不生成虛構 calibration 標籤。
2. P1：對完成回歸的代表性 IP 做 mapping sweep；先 formal／獨立 scoreboard，再比較資源、時序與 bytes。Minisim 不支援的 RTL 仍回報 unsupported。
3. P1：加入合法的小器件候選與真實板卡 PCF，跨 seed 比較；完整 IO timing 未約束時明說。
4. P2：ECP5／Gowin 壓縮與封裝路線分開驗收；板卡未指定前不寫入硬體。
