# VHDL 與混合語言：第一個已驗收子集

2026-11-01。使用者要求 Minisim 支援 VHDL 與混合語言模擬，本輪已實作一條可執行的受限路線，而非只有環境安裝或轉檔：

`VHDL RTL → GHDL synthesis Verilog → 與 SV wrapper 一起經 Yosys flatten → pyslang netlist IR → Minisim event kernel`

實際案例是 VHDL 同步-reset counter 的輸出送入 SystemVerilog 加法器。netlist 前端新增 continuous wire drivers、ternary mux、多輸入 sensitivity 及 flatten 後的 escaped signal names，並拒絕延遲／strength drivers、combinational cycles 與混合 procedural/continuous writers。原有有限 RTL 前端維持不變，避免正在進行的 portfolio 相容性探測途中更換版本。

## 實際結果

- GHDL 原生 VHDL-2008 testbench 跑 40 cycles，對 counter 使用獨立 golden assert，reset／計數／wrap 均通過。
- 正向 mixed design 在 Minisim 跑 40 筆，與 Icarus 執行相同混合來源的轉譯結果逐筆一致，也符合由原生 VHDL trace 加 SV 規則得到的 golden。
- SV 運算改為加 2 的 mutant 同樣跑 40 筆；Minisim／Icarus 仍一致，但被原來的 mixed golden 拒絕。兩個 case 共 **80 筆**參考一致。
- 3 個 netlist frontend 單元測試通過，包括 sensitivity／mux、escaped names，以及 cycle／delay／strength 拒絕檢查。
- line bins 是生成 netlist 的行映射，**不是原始 VHDL 行覆蓋率**。原始 VHDL、SV wrapper 與前端 SHA 保存在 summary。

產物：`build/coverage/mixed-language/summary.json`，以及 GHDL 原生模擬 log、轉譯 Verilog、flattened SV、IR/C++、Minisim/Icarus traces、line.json。GHDL 4.1.0、Yosys 0.33 及 pyslang 12.1.0 在獨立 portfolio runtime 中實跑，沒有宣稱新版文件上的所有功能均支援。

重跑：在有 GHDL、Yosys、G++、Icarus、pyslang 的 Linux 環境執行：

```sh
python -m unittest discover -s tests -p test_netlist_frontend.py -v
python tests/run_mixed_language.py --output build/coverage/mixed-language
```

## 明確限制與後續

這是 **可綜合 VHDL＋SV 的明確轉譯／flatten 執行子集**，目前只驗收 SV top→VHDL 子模組。尚未完成原生 VHDL parser、九值 std_logic、完整 VHDL delta-cycle/testbench semantics、跨語言任意 hierarchy binding、反方向 VHDL top→SV 子模組或完整 assertion/PSL。不能將這些未完成項目標成支援。

下一步：

1. 增加反方向 binding 與多時脈／reset 介面契約，保留原始來源至 netlist 的可追溯關係。
2. 明確處理／拒絕 weak、high-impedance、don't-care 值，加入 GHDL 原生 delta-cycle 對照；不可在轉譯中靜默改變九值語意。
3. 使用具原始 upstream testbench 的 VHDL OpenCores IP 擴大驗證；先建立小型測試，再談完整 mixed-language kernel。
4. 持續完成 SVA AST／sampling 與先前 portfolio 全量回歸，保留各路線的支援與未支援清單。

技術來源：[GHDL Synthesis 官方文件](https://ghdl.github.io/ghdl/using/Synthesis.html)，其中明確區分 synthesis netlist 與原生模擬，並標示 synthesis 為實驗功能。
