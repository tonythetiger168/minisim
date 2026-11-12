# Minisim 深化計劃

更新：2026-11-01。基線 main@fe6551c；本地工作分支 codex/minisim-four-state-mux，未推送。此專案是實驗性的四態事件模擬器；新增 Python/pyslang 有限子集前端已完成 counter 端到端驗證，C++ slang lower.hpp 仍未編譯驗證。Helios 不能把它視為通用 RTL backend 或獨立 correctness oracle。

## 已完成與證據

使用者新增 Basanos 整合：相鄰 repo 基線 293beaf，本地 codex/ai-eda-integration。已用 Icarus／pyuvm 跑通 TinyALU 500 筆並經 Dynamis／Helios TX 閘門；Minisim 實際 lowering 仍為 unsupported（module member Parameter），不標原生 PASS。詳細版本、coverage gate 修正與 formal 缺口見相鄰 basanos/INTEGRATION.md；下一步須補 parameter／async reset／case／運算與通用 DUT manifest。

最新整合：實驗性 counter VPI host 已執行真正 cocotb clean 96-cycle PASS，+2 mutant 在 15 ns／sample index 1 拒絕；沿用原 pyuvm scoreboard 也通過 96 samples／3 wraps／7 resets，mutant 在 sample=2 拒絕。詳見 PYTHON_BRIDGE.md，並非任意 RTL 通用 backend。另新增 Dynamis TX 實測整合（9 個正反例閘門、3 HTML）與 Helios evidence gate（5/5），見 [DYNAMIS_INTEGRATION.md](DYNAMIS_INTEGRATION.md)。使用者將跟進頻率改為每 30 分鐘，既有 automation 已更新，不建立重複 job。

台北 2026-11-03：完成 cocotb 2.1.0 實際 VPI imports 盤點（16 個）與系統 VPI header compile probe 1/1。下一步選擇 VPI adapter route，因安裝 wheel 無 GPI private class header；詳見 PYTHON_BRIDGE.md 與 build/cocotb-interface.json。尚無 adapter／cocotb 動態通過，不將 ABI 盤點當作整合完成。

使用者要求 Next phase：原生 timer／edge／ReadWrite／ReadOnly callbacks 已實作，含 once／取消／callback 預算／ReadOnly 寫入限制／禁止重入；C ABI 可由 Python 註冊。核心 20/20、真實 generated counter 4 週期／16 callback 順序通過，三 RTL 版本 288 筆仍和 Icarus 一致。詳見 [PYTHON_BRIDGE.md](PYTHON_BRIDGE.md) 第二階段。下一步為真正 cocotb GPI/VPI adapter 與 regression 啟動，仍未宣稱支援 cocotb／pyuvm。

使用者要求接續 cocotb／pyuvm 整合：第一階段完成外部時間推進 `advance_to`、同時刻外部事件與 NBA 排空、單次 VCD header、Python ctypes counter fixture。真實 pyslang 降低的 clean／width8／increment2 各 96 筆，合計 288 與 Icarus 一致；核心 19/19 通過。尚無 cocotb callbacks／scheduler 或 Minisim pyuvm 執行。下一個優先工作為 GPI/VPI callback 順序契約與實際 cocotb counter，見 [PYTHON_BRIDGE.md](PYTHON_BRIDGE.md)。

2026-11-02 使用者跟進：完成 Task exception_ptr 傳遞與 Delay uint64 overflow 檢查。程序錯誤含 time／原因且停止待執行 NBA；最大合法 timestamp 可排程，回繞則拒絕。核心 19/19 通過，counter 11 changes／finish t=122 和 Icarus 一致。介面與限制見 [KERNEL_LIMITS.md](KERNEL_LIMITS.md)。下個核心優先項是 VCD identifier 超過 94 訊號的合法編碼與 reader 驗證，再接回可重用 DUT／來源映射整合；不得將這項防護說成增加 RTL 語法支援。

2026-11-02 14:58 UTC：新增 `max_ir_instructions_per_resume`（預設 1,000,000，正整數），限制 `exec` 每次等待之間的 IR 指令數。超限在執行／coverage 計入下一條指令之前停止，透過 `execution_error` 與 `Sim::run` 回報時間及 PC，避免從 coroutine 拋出而觸發現有 terminate。新增不等待的自跳轉拒絕測試與跨 20 time steps 的合法 wait 迴圈測試，完整核心 **16/16** 通過；證據 build/ir-budget-evidence.json。這未攔截自訂 native C++ coroutine 的不 yield 迴圈，亦未普遍修復 coroutine exception 傳遞或 time overflow。

2026-11-02 13:56 UTC：新增每個 simulation time slot 的 coroutine resume 上限 `Sim::max_resumes_per_slot`（預設 1,000,000，可設正整數）。超限將 finished 設 true 並拋出含目前時間的錯誤；零上限拒絕。實際 #0 無限迴圈在 time 0 被阻止，另一個每時槽上限 1、跨 20 個時間步的有限程序正常結束，證明計數隨時間槽重設。核心 runner **14/14** 通過，包含既有四態、NBA、finish、coverage/property 測試。此保護只涵蓋有讓出控制權的 coroutine，尚未阻止單次 resume 內的無限 C++／IR 迴圈，亦未處理 time overflow；不是完整 sandbox 或 delta-region 合規驗收。

- 修正未知條件 mux 的共同 Z 被轉為 X。先跑差異測試：1,024 個二位元案例有 62 個與真實 Icarus 不符；修正後為 0。另測 64-bit 全 Z 邊界。測試的兩分支等寬，未驗證所有 expression sizing 情境。
- 修正 $finish 後仍消耗 active/NBA 佇列，以及 IR 在 Finish 指令後繼續執行。
- VCD 寫入初始值，即使訊號一直未改變也可觀察。
- 新增 7 個核心情境，連同 test_expr、test_z、test_mux 共 10 個測試入口。修正核心前 4 個情境失敗；修正後全數通過。
- 比較手工 counter demo 與 Icarus 執行 examples/counter.sv：11 次數值變動、t=122 結束一致。這是排程及手工 IR 的特定整合案例，不是 slang 降低流程驗收。
- CMake 登錄新測試並隔離各 kernel scenario 的波形輸出目錄；Release 也保留原有 assert 檢查。後續已安裝 CMake 4.4.4 並實際完成 Release 建置、CTest 12/12 通過。

實際環境：WSL Ubuntu、G++ 15.2.0、Icarus 12.0。重現：

```sh
python3 tests/run_core_tests.py
python3 tests/compare_mux.py
python3 tests/compare_counter.py
```

測試包含 NBA 與 #0 先後、部分位元 NBA 合併、多事件同時命中只喚醒一次、0→X 與 Z→1 正緣，以及停止與初始波形案例。新測試以顯式失敗碼運作；既有 assert 測試由 runner 加 -UNDEBUG 保留。

## 後續優先順序

新增 FPGA 需求：GitHub 官方工具 12 項已鎖定，目標為功能與時序約束下的最低資源／最小 bitstream Pareto 候選。iCE40 已完成三設定各 300 筆 gate-level 比對、P&R 與 bitstream roundtrip；資源 11/11/12 LC，binary 皆 32220 bytes。完整範圍與後續驗收見 [FPGA_TOOLS.md](FPGA_TOOLS.md)，不宣稱全域最小或完整 Minisim FPGA backend。

最新跟進：已驗收受限 VHDL→Verilog＋SV flatten→Minisim 路線，80 筆與 Icarus 一致並有 GHDL 原生 golden；仍非完整原生 VHDL／九值或任意 mixed-language 支援，見 [VHDL_MIXED_LANGUAGE.md](VHDL_MIXED_LANGUAGE.md)。使用者另指定 OpenTitan，已 clone 並鎖定 41049e9；代表性 GitHub/OpenCores 清單與順序見 [REPRESENTATIVE_IP_PROJECTS.md](REPRESENTATIVE_IP_PROJECTS.md)。

使用者最新需求：先完整跑 129 IP 與 130 VIP，再加代表性 GitHub/OpenCores IP；Minisim 另需支援 VHDL 與 mixed-language simulation。執行狀態與驗收階段見 [PORTFOLIO_REGRESSION_PLAN.md](PORTFOLIO_REGRESSION_PLAN.md)。目前未宣稱 Minisim 已有原生 VHDL／mixed-language 支援。

14:05 台北時間排程跟進：已將 live coverage-driven 刺激與真正 pyslang-lowered RTL 接通。4-bit／8-bit 分別 20／260 cycles 閉合，+2 mutant 在第 3 sample 被 golden 拒絕，reset-only 案例於 8 cycles 回報未閉合；291 筆記錄與 Icarus 及 Verilator 同時一致。模型與執行引擎的版本／檔案 hash 保存在 manifest。詳見 [ADAPTIVE_FRONTEND.md](ADAPTIVE_FRONTEND.md)。此里程碑不代表完整 SVA 或任意 DUT closure；下一輪優先 AST/property sampling 契約。

13:01 台北時間排程跟進：已接通 pyslang AST→來源映射 IR→generated C++→Minisim 實際執行；三個 RTL 版本合計 288 筆與 Icarus 一致，5 個 Python tests（含 16 類拒絕案例）及 Release CTest 12/12 通過。詳見 [PYSLANG_FRONTEND.md](PYSLANG_FRONTEND.md)。下一步為 SVA 支援子集的 AST／sampling 契約、kernel 抽離及可重用刺激，不把此有限前端或 counter harness 當作完整 SVA／任意 RTL 支援。

使用者新增 coverage-driven 重點：line、FSM、toggle、SVA assertion、functional SVA。已實作原生 mapped-IR line／toggle／FSM／functional-bin／clocked-property API，coverage-driven counter 20 cycles 填滿 20 個功能 bins；Verilator 5.032 真實 RTL line/toggle 與 assert/cover property 參考驗證亦通過 clean、no-start、指定 assertion mutant 三種情境。範圍、限制與重跑方式見 [COVERAGE.md](COVERAGE.md)。下一輪優先連接 pyslang 來源映射與可執行 IR，擴展 coverage fixture 到可重用 DUT 介面；完整 SVA parser 尚未實作。

同日新增驗收：pyuvm 5.0.0／cocotb 2.1.0／Icarus 12.0 的真實 counter 測試已接通。`tests/run_pyuvm_counter.py --check-mutant` 驗證 96 cycles、3 次 wrap、7 筆 reset、全部 16 個數值，並成功拒絕加 2 的 mutant（第 2 sample）。也檢查同步 reset 不在無時脈邊緣時修改 cnt。這是獨立 RTL reference，不是 Minisim 的 VPI 支援；後續跨 backend 應重用刺激與逐筆交易契約，再進行一致性比較。

| 優先 | 工作 | 驗收標準 |
|---|---|---|
| P0 | 擴充四態差異測試 | 針對 shift、mixed width mux、case、wire resolution 做獨立 Icarus 比較；每個差異先保留最小重現，再修正 |
| P0 | 排程健全性 | 加入零時間無窮迴圈／事件數上限、時間加法 overflow 檢查；錯誤需有時間及原因，正常案例不受影響 |
| P0 | 核心與 demo 分離 | 將 Sim、awaiter、exec 抽離到可獨立使用的核心介面，避免測試 include .cpp；保留現有全部測試與 demo 差異結果 |
| P1 | 波形完整性 | 超過 94 個訊號時改用多字元 VCD identifier；驗證 X/Z、向量、時間單調、平行測試隔離及標準 reader 可讀 |
| P1 | 可重現 slang frontend | 先選定並鎖定可編譯版本，編譯 slang_dump/lower.hpp；counter.sv 端到端產生 IR，再對照手工 IR 和 Icarus |
| P1 | 跨平台建置 | 實跑 CMake Debug/Release；清除 GNU 專屬 builtin（例如 parity）後驗證 MSVC，不能因 C++20 宣告就稱 Windows 相容 |
| P2 | Helios shadow backend | 真正支援 RTL 輸入後，建立明確的支援子集及 unsupported 結果；同一 SHA／刺激下比對交易或波形，不能用 TEST PASS 字串代替語意比對 |

## 已知缺口

尚未驗證任意 RTL、UVM、階層、signed、完整 event-region 語意或商業模擬器相容性。slang 未接通前不累積「Minisim 驗證通過」的 Helios 校準標籤。所有修復目前僅在本地，沒有推送、合併或發布。

研究參考：[Verilog-AMS expression 文件](https://www.verilogams.org/refman/basics/expressions.html)。正確性判斷另以實際 Icarus 執行的差異結果佐證，並未聲稱完成 IEEE SystemVerilog 全面符合性驗證。


## 2026-11-03 常數參數里程碑

新增elaborated default parameter/localparam到常數IR，含可驗證metadata與unsigned高位邊界。前端7/7，四種參數化counter 384筆與Icarus一致。Basanos下一阻礙為async-reset多事件（single clock edge required），不再停在Parameter，但仍unsupported。下一P0為明確設計多事件/reset sampling契約及差異測試；不先跳過未知語法。原kernel未改，不重複其20項測試。詳見PYSLANG_FRONTEND.md與build/parameter-frontend/summary.json。


## 2026-11-03 03:19 Taipei：coverage後端升級評測設計

官方Verilator修訂紀錄標示5.054發布／事件日期2026-11-01；查閱2026-11-03，屬7日窗，不冒充最近24小時發布。來源：https://verilator.org/guide/latest/changes.html 。官方列出line與branch coverage旗標分離、FSM state/arc精度、non-overlapped implication與VPI支援改進；這些是上游描述，本地尚未驗證5.054。中文OSCC/AiEDA官網搜尋未取得可核實的新增24小時發布，不重計舊成果。

P0仍為完成portfolio：本輪Docker可查詢，原容器exited，host可用1808664 KiB（約1.73 GiB）；未恢復2304MiB上限編譯。progress仍記vip-coverage running是停止前checkpoint，不能當活躍job；vip-coverage與independent-crosscheck尚未完成，未新增動態PASS。保留既有結果及其他專案。

P1為5.052→5.054版本對照（效益高／成本中／風險中，可驗證）：另建隔離後端，不覆寫現有版本。驗收先選原clean／no-start／assertion-mutant及一個明確分支RTL；固定來源SHA、刺激、工具版本及旗標。5.054明確分開啟用line和branch，保存各自非空分母、命中及holes，不把兩版不同分母直接算成改善。FSM保存宣告state/arc和實際命中；SVA保留pending obligation、vacuity及mutant拒絕證據。clean行為須一致，no-start不得虛假closure，mutant須失敗；若上游語意修正造成差異，先最小重現再判定。尚未安裝或跑5.054，測試新增0。

P2仍為完成兩個portfolio後跑OpenTitan六IP與四個OpenCores上游bench；來源清單見Minisim docs/EXTERNAL_IP_INTAKE.json。這些是測試／評測資料準備，不是模型訓練完成。沒有啟動付費API、推送或合併。


## 2026-11-03 04:49 Taipei：async-reset參考契約

新增Minisim tests/async_reset_contract.sv，由現有Icarus實跑6/6檢查：無clock的reset assertion、release不增值、正常clock、同時clock/reset以reset優先、後續clock與再次reset。明確1ns/1ps，NBA後取樣；這是二態已知刺激的參考語意，不是Minisim PASS，也不是任意同時事件順序的完整證明。kernel已有WaitAny共享token；pyslang frontend仍只接受SignalEvent，不能把event list拆成連續WaitEdge，否則變成先後等待。下一步需接通event-list IR／C++發射及同時事件只喚醒一次、重新arm的差異測試，再處理Basanos後續語法。

本輪不改kernel，不重跑既有counter；Docker仍exited，可用1426624KiB約1.36GiB，未恢復大型回歸。portfolio未完成，OpenTitan/OpenCores保持既定次序。研究依據為本地實際事件程式及Icarus參考執行；未宣稱新增產業發布。未推送。


## 2026-11-03 05:19 Taipei：pyslang多事件IR

前端現在接受由一位元模組訊號組成的明確posedge/negedge event list，去除重複項後產生單一WaitEdge與events，C++ emitter寫入既有Instr.events，沿用WaitAny共享token。level event、iff及複合事件表達式仍拒絕，未新增kernel語意。新增tests/test_event_frontend.py三項檢查通過：雙正緣／正負緣IR及header、拒絕level event。尚無本輪native C++或Icarus差異結果，不稱async-reset整合完成或Basanos PASS。下一步編譯生成header，測試同時觸發只喚醒一次與再次arm。保留既有參數修改，未推送。


## 2026-11-03 05:49 Taipei：多事件原生差異驗證

新增tests/run_event_differential.py，用pyslang生成header、G++單工-O0編譯真實Minisim kernel，對照Icarus同一RTL／刺激。async reset 8筆及雙event同時觸發8筆，16/16完全一致；包含reset assertion/release、同時觸發只增值一次與後續重新arm。保留source SHA、reference/native logs及summary於build/event-differential。輸出X/Z直接失敗，非空樣本檢查。核心未改，證實既有WaitAny可承接新前端；只涵蓋已知值、小型flat RTL，不是完整event-region或任意reset時序證明。尚未驗收Basanos ALU，新下一步應重新probe其後續unsupported語法並擴充負例／mutant。portfolio本輪未重啟，OpenTitan/OpenCores尚待既定順序。未推送。


## 2026-11-03 06:19 Taipei：Basanos下一阻礙實測

以最新pyslang前端直接probe真實basanos/hdl/tinyalu.sv，診斷已由single clock edge required前進到expression UnaryOp（!reset_n）。來源與前端SHA保存Minisim build/basanos-event-probe/summary.json；尚無ALU native模擬。kernel expr.hpp已有Op::LogNot/v_lnot，下一P0為前端UnaryOp(LogicalNot)至un(Op::LogNot,operand)映射，保持bitwise-not與logical-not不同，不對X/Z做Python布林轉換。驗收需1-bit 0/1/X/Z及多bit已知1混X的truth規則，和Icarus差異比較及mutant拒絕，再重新probe case、AND/XOR/MUL與context width，逐項列阻礙。先診斷再擴充，未默默改寫Basanos RTL避開語法。

本輪新增一次真實lowering probe，結果unsupported，不計RTL PASS。原多事件16筆差異結果沿用，無故不重跑；Docker仍exited，portfolio／外部IP測試尚未完成。研究依據為本地前端、核心與RTL的直接證據，不重計舊Verilator發布。未push/merge。


## 2026-11-03 06:49 Taipei：四態logical-not接通

前端新增UnaryOp LogicalNot→LogNot IR與un(Op::LogNot,operand) C++映射，其他unary仍拒絕。生成header後編譯真正kernel，0/1/X/Z四筆分別得到1/0/X/X，4/4與Icarus一致。保存tests/test_lognot_native.py及build/lognot-native；核心未改。這只涵蓋1-bit logical-not，寬向量混X及mutant仍待補，不宣稱完整unary支援或ALU PASS。下一步依重新probe繼續case與算術，保留已驗收多事件。Docker本輪仍exited，portfolio未重啟；OpenTitan/OpenCores未外推PASS。未推送。


## 2026-11-03 07:19 Taipei：logical-not寬向量與mutant

新增tests/test_lognot_wide.py，四位元0/1/X/Z全部256組，真實pyslang header→G++→Minisim輸出256/256與Icarus一致，涵蓋已知1混X/Z、全0混X/Z及全未知。將生成header的LogNot改成Not重新編譯，bitwise-not mutant輸出不等且被oracle拒絕，最後還原header。build/lognot-wide保存完整rows與summary；不改kernel，未宣稱其他unary或Basanos case已支援。測試可在WSL既有venv執行；Basanos下一阻礙仍Case。portfolio本輪未啟動，OpenTitan/OpenCores依既定順序保留。未推送。


## 2026-11-03：Basanos TinyALU 真正Minisim原生執行

使用未修改的basanos/hdl/tinyalu.sv（SHA 3f41876c871b78e94a43506d99bea2cf3b1c440f457631452485406c9cd159a6），pyslang→IR→C++生成→原生Minisim執行，535筆result/done同時與Icarus及獨立Python ALU oracle一致。含32筆opcode/邊界定向刺激、500筆固定seed隨機／reset／start保持及2筆X/Z opcode default，加初始reset。乘法改加法、case比較改不等兩個生成程式mutant均被拒絕，header最後還原。證據在minisim/build/tinyalu-native，重跑minisim/tests/run_tinyalu_native.py（WSL既有venv，G++/Icarus）。

前端新增普通case使用CaseEq精確比對、first-match跳轉、分組標籤與default/no-default；只接受已知常數標籤，拒絕casez/casex/unique/priority與X/Z標籤。case frontend3/3。加入BinaryAnd/BinaryXor/Multiply映射至既有kernel運算；unsigned已知常數不再因最高位為1誤判負數。移除舊unit中已過時的多事件拒絕預期，保留其專門測試。kernel未修改。

這是TinyALU direct native harness的實際通過，不是將原500筆Icarus/pyuvm結果冒充Minisim，也不是通用cocotb/VPI或Basanos pyuvm suite已接上Minisim；該整合仍待泛化DUT handles／manifest。完整coverage closure、formal無界proof、任意case語意及其他IP仍未完成。下一步補分組/default/hold動態差異與擴充可重用DUT bridge；既有portfolio優先／OpenTitan及OpenCores次序保留。不push/merge。


## 2026-11-03 07:49 Taipei：case動態邊界

新增tests/run_case_dynamic.py：兩個真實RTL生成程式，default置於首項仍優先匹配明確標籤、分組標籤，以及無default保持前值；各含兩位元selector全16種0/1/X/Z組合與一次初始已知命中，共34/34筆native與Icarus一致。保存build/case-dynamic雙方log及summary。原TinyALU535筆與兩個mutant結果不重跑；casez/casex/未知標籤仍拒絕。下一步泛化VPI DUT介面，不能把direct harness稱成TinyALU pyuvm已接通。容器仍exited，可用1049332KiB約1GiB，本輪未恢復全量編譯。未推送。


## 2026-11-03 08:19 Taipei：真實cocotb TinyALU VPI

新增experimental vpi_generic_main.cpp，以vpi_manifest.hpp設定top與output清單，root/full-name/lookup及direction不再固定counter/cnt；缺manifest仍保留counter預設。使用相同真實cocotb2.1動態VPI library與原生TinyALU生成程式，128筆result/done非空scoreboard與XML testcase PASS，涵蓋8種opcode、reset與保持值。測試及runner為tests/cocotb_tinyalu_native.py與run_alu_vpi.py，證據build/alu-vpi。這是新TinyALU cocotb測試，不是原Basanos pyuvm suite；未作此VPI路徑mutant拒絕，亦不是通用RTL/VPI支援。manifest目前手動設定flat訊號方向，尚未自動由port AST生成／完整驗證；原counter host保持不動。下一步先做同一路徑mutant，再接原pyuvm，補共用host抽離以避免兩份實作分歧。未push/merge。


## 2026-11-03 08:50 Taipei：原Basanos pyuvm接通Minisim

沿用已通過TinyALU cocotb的相同Minisim VPI binary，載入原basanos/testbench.py（無另寫簡化UVM替身），pyuvm5/cocotb2.1實際執行500筆scoreboard、0 errors；XML單一testcase PASS，tx.jsonl恰500筆。證據minisim/build/basanos-pyuvm-vpi，runner tests/run_basanos_pyuvm_vpi.py；先以run_alu_vpi.py建置binary。

另以相同cocotb ALU測試，生成header將Mul改Add，XML明確1 failure含ALU mismatch sample，非bootstrap失敗；runner run_alu_vpi_mutant.py及build/alu-vpi-mutant保留證據。這項mutant是128筆cocotb測試的負例，尚不是原500筆pyuvm suite的mutant，不混稱。原pyuvm clean本輪500筆已真實通過，仍限flat TinyALU VPI host；未有完整任意DUT、階層、DPI、coverage closure或商用模擬器相容性。下一步將此500筆交易接Dynamis／Helios gate與Icarus seed0交易對照，再補原suite mutant及host共用化。未push/merge。


## 2026-11-03 09:20 Taipei：Minisim／Icarus pyuvm交易證據鏈

將原Basanos pyuvm在Minisim VPI產生的500筆交易對照既存Icarus seed0交易：payload與t逐筆一致，兩檔SHA256皆0c5ee26b23a01783bc0cd0f68779771b1ce0cc8dc289ee0e869af4ae2ee32224。另以獨立Python ALU式逐筆檢查500筆結果，五種operation皆出現。新增tests/compare_pyuvm_transactions.py，實際呼叫Dynamis/Helios --with-time四關卡：clean equivalent、錯值／缺筆／時間+1均different，4/4符合預期；每個gate保留input/tool SHA與Dynamis revision，不生成calibration label。

證據minisim/build/pyuvm-cross-backend。這次重用已保存的Icarus baseline，沒有新跑Icarus，也不宣稱一般設計證明；三項負例是交易檔mutant，不是原pyuvm DUT mutant。下一步對原pyuvm suite注入DUT錯誤及整合coverage holes，抽離重複host程式。原portfolio與OpenTitan/OpenCores排程不變，未推送。


## 2026-11-03 09:50 Taipei：原pyuvm DUT mutant拒絕

原Basanos testbench.py在相同Minisim VPI adapter上執行multiply-as-add DUT binary，完整500筆scoreboard與500筆TX，發現97錯；XML單一failure明確包含mismatches!，無error/skipped，runner要求此預期拒絕。這是DUT運算mutant，不是僅修改交易檔或換testbench。保存build/basanos-pyuvm-mutant的XML/log/TX/summary及testbench/binary SHA，重跑tests/run_basanos_pyuvm_mutant.py（先run_alu_vpi_mutant.py建置錯誤binary）。正常500/0與此500/97分開留存；不將預期failure標成正常DUT PASS。

至此原suite已有正常、DUT負例及跨Icarus時間/資料比對；仍非完整coverage closure或通用VPI。下一P0為從已存在交易計算明確functional bins/holes，再針對未命中刺激閉環；line/toggle/FSM/SVA需各自原始分母，不能用交易bin代替。背景portfolio仍exited，尚未完成VIP coverage／crosscheck。未推送。


## 2026-11-03 10:20 Taipei：TinyALU functional holes

新增tools/alu_functional_holes.py，以已驗證的500筆交易計算固定55個分母：5 opcode，加每opcode與a/b各5個corner（0/1/127/128/255），非a×b joint cross。實測21/55命中、34 holes，closed=false；產生去重directed_stimuli但本輪尚未執行。計算前逐筆獨立ALU檢查，拒絕錯誤樣本／空輸入／非法欄位，保存schema與來源SHA，不以空分母宣稱closure。tests/test_alu_holes.py三項實際證據檢查通過：500筆正常仍未閉合、原DUT mutant拒絕、空輸入拒絕。證據build/alu-functional-holes.json。

下一步透過已驗證cocotb VPI執行生成刺激並重新驗證結果後合併相同schema計數；本輪新增RTL模擬0，不能宣稱34 holes已補齊。這只是transaction functional coverage，不代替line/FSM/toggle/SVA或完整cocotb-coverage套件。未推送。


## 2026-11-03 10:50 Taipei：holes驅動定向實跑閉環

使用上一輪34 holes產生的31筆去重刺激，透過真正cocotb／Minisim VPI binary執行，逐筆獨立ALU expected/result與done核對，XML PASS。相同schema與55個固定分母：原500筆21/55，加31筆後531筆55/55、holes=0。runner先核對plan來源SHA，拒絕stale plan；分析器仍拒絕空或錯誤交易。證據build/alu-holes-vpi含before/after、XML、log、directed/combined JSONL及binary/RTL/model/plan SHA。重跑tests/run_alu_holes_vpi.py，先準備原500筆與holes plan。

這個closure僅5 opcode與op×個別a/b corner（非a×b joint cross），不代表line/FSM/toggle/SVA／完整functional closure。directed交易t使用刺激索引，combined是獨立run的coverage輸入，不能拿它當單一全域時間軸做時序等價。原500筆Icarus/Minisim含時間等價證據保持獨立。下一步接原生line/toggle量測並抽離共用VPI host；先核對資源才能續跑portfolio，OpenTitan/OpenCores未因此完成。未推送。


## 2026-11-03 11:20 Taipei：VPI原生line與toggle

experimental generic host新增MINISIM_COVERAGE opt-in，必須在build_generated_design前啟用才註冊signal toggle分母；結束寫line.json/toggle.json並檢查開檔／寫入失敗。tests/run_alu_vpi_coverage.py用真實cocotb128筆通過XML與scoreboard，得到line12/12、directional known0/1 per-bit toggle76/78；剩sig:6:bit:15的rise/fall，不宣稱toggle closure。保存build/alu-vpi-coverage原始bins及holes。

line是pyslang對支援IR executable lines的映射，不是全RTL語法／branch coverage。toggle包含本flat DUT所有登記訊號，X/Z不算known0/1轉換；兩者均非FSM或SVA。functional55/55結果獨立。下一步將sig索引對映RTL來源名稱、確認可達後補bit15刺激，並保留baseline與新增run schema/來源一致性。kernel本輪未改；未推送。


## 2026-11-03 11:50 Taipei：directional toggle holes閉環

依generated design.hpp註冊順序確認sig6=result（16bit），未命中為result[15] rise/fall。使用相同coverage-enabled VPI binary執行255×255=65025與NOP回零兩筆定向刺激，真實cocotb XML PASS，result/done scoreboard正確；新run原始bins兩個方向皆命中。與原76/78基線以相同78個key合併後78/78、holes=0；不是新兩筆本身涵蓋全部訊號，保留new_run_closed。build/alu-toggle-holes保存XML/log/原始line與toggle/merged bins及binary SHA，重跑tests/run_toggle_holes_vpi.py。

目前TinyALU受限scope：IR來源映射line12/12、跨run directional toggle78/78、transaction functional55/55，各分母獨立；不宣稱branch、FSM、真正RTL SVA parser或完整RTL closure。後續先抽共用VPI host與port manifest自動化，再針對FSM/SVA選實際有狀態／property的RTL，無對應結構不可硬製造分母。portfolio／OpenTitan/OpenCores仍未完成，未推送。


## 2026-11-03 12:20 Taipei：AST自動VPI manifest

新增tools/vpi_manifest.py，從pyslang elaborated PortSymbol讀top／direction／width，核對generated signal寬度與internalSymbol同名；限制input/output，拒絕inout/ref/alias與空介面。保存schema與RTL SHA；目前flat descending known width支援子集不變，沒有新增runtime VPI能力。TinyALU8 ports、result/done輸出、counter輸出與inout拒絕共3項檢查通過。

run_alu_auto_manifest.py以自動header取代手工output清單，真實cocotb128筆scoreboard與XML PASS；證據build/alu-vpi-auto-manifest含manifest.json/header。既有手工runner仍保留，下一步抽共用build/run入口並將其他profile切換自動manifest，不能宣稱所有舊runner已更新。原line12/12、toggle78/78與functional55/55各自沿用先前證據，非這128筆全部closure。未推送。


## 2026-11-03 13:20 Taipei：共用VPI host

將tests/vpi_counter_main.cpp縮為相容入口include vpi_generic_main.cpp，counter與TinyALU現在使用同一份VPI imports／handles／callbacks／coverage實作；保留counter預設top/output，現有build入口不變。修改前host備份與SHA在build/vpi-shared-host。以原run_cocotb_minisim.py重新編譯共用host，clean96筆0fail；increment2 mutant仍由預期counter mismatch拒絕。證據build/vpi-shared-host/counter-clean/summary.json與各XML/log。

這次因host整理重新驗證必要counter相容性，不重跑無關kernel/RTL回歸。TinyALU runner已原本使用generic host，尚未將所有Python build/run複製段抽離；下一步集中環境／XML驗證與provenance。未增加任意DUT/階層/VPI能力，不推送。


## 2026-11-03 13:50 Taipei：VPI來源校驗

新增tools/vpi_provenance.py，成功build路徑記錄binary與RTL、generated design/manifest headers、VPI host、frontend、test module與核心hpp/inc/cpp SHA256；auto-manifest runner在模擬前後verify，來源或binary內容改變則拒絕。tests/test_vpi_provenance.py三項測試通過（含逐一篡改binary/RTL/header、空輸入及缺檔拒絕）。整合後重新編譯真實cocotb TinyALU128筆與XML PASS，provenance.json在build/alu-vpi-auto-manifest。

這是檔案內容一致性檢查，不是簽章或hermetic build證明；目前未鎖compiler／外部library版本與所有include追蹤，也未將其他舊runner接入。不能事後對舊binary捕捉目前來源就宣稱build因果證明。下一步集中build/run共用函式與套件/tool identity，逐個profile接入；不擴大原coverage或VPI能力宣告。未推送。


## 2026-11-03 14:20 Taipei：共用XML驗證

新增tools/vpi_results.py並接入auto-manifest runner，取代重複inline XML判斷；要求非空且預期數量testcases，拒絕error/skipped/unexpected failure，負例需明確指定scoreboard字串與恰一個failure。四項unit測試通過，另讀既有真實TinyALU clean、原pyuvm DUT mutant、counter mutant XML，3/3符合預期；證據build/vpi-result-checks。本輪新增模擬0，只因純結果解析抽離而驗證既有XML，不重跑已通過RTL。其他runner尚未全面遷移；build/env共用化及tool identity仍待完成，不宣稱整個pipeline已重構。未推送。


## 2026-11-03 14:50 Taipei：共用runtime與工具身分

新增tools/vpi_runtime.py，統一real cocotb VPI環境與flat top/module名稱檢查；載入實際libpython/cocotb VPI library，缺檔直接失敗。auto-manifest runner改用共用environment，保存runtime.json：Python可執行檔／版本、cocotb/pyuvm/pyslang版本、G++路徑與版本、兩個library SHA；不保存完整環境變數。library與runtime helper加入provenance模擬前後驗證。整合後真實TinyALU cocotb128筆與XML PASS。

這是runtime身分記錄與內容校驗，尚未鎖定整個compiler binary／系統headers或提供hermetic build；其他runner仍待遷移。主機可用1550824KiB約1.48GiB，portfolio容器exited，本輪未恢復大型編譯。下一步集中成功build入口／快取失效條件，減少重複的實驗runner；不改coverage分母或擴大VPI能力宣告。未推送。


## 2026-11-03 18:50 Taipei：VPI四態實測

新增tests/cocotb_alu_fourstate.py與run_alu_fourstate.py，沿auto-manifest binary先驗證provenance，經真實cocotb VPI寫入並讀回8bit X/Z與mixed 10xz01z0，AND零遮罩、XOR零與MUL零，共15個輸出與Icarus一致、XML PASS。包含0*X仍未知、Z運算轉X而input roundtrip保留Z，避免只做known-value驗證。使用共用environment/XML/provenance helpers，未重編譯C++；保存build/alu-fourstate雙方輸出與trace。

這只涵蓋上述flat TinyALU寫讀／運算，非所有VPI format、64bit、任意四態scheduler或其他DUT完整支援。kernel/host未改，無故不重跑原535筆；下一步按實際待辦處理共用build與更多handle/format負例。portfolio未啟動，未推送。


## 2026-11-03：共用build入口與VPI介面負例驗收

新增tools/vpi_build.py提供build(source, output, test_files)與CLI，集中pyslang lowering、自動port manifest、單工G++編譯、build.log/命令/runtime與provenance；編譯至simulator.building，成功且input hashes未改變才替換simulator，不隱式沿用舊binary。紀錄compiler executable、來源/核心/host/library/helper/test SHA；仍非完整hermetic build。run_alu_auto_manifest.py移除內嵌build，run_vpi_negative.py亦使用同入口，舊實驗runner尚未全面遷移。

實測：共用build clean cocotb128筆/XML PASS；新binary四態VPI15筆與Icarus逐筆一致，input X/Z保留。新增tests/vpi_negative_main.cpp/run_vpi_negative.py直接呼叫真實host VPI exports，output write、nonzero delayed deposit、force、invalid binary、indexed handle、不支援read format六項均明確拒絕，missing-name回傳null，7/7符合預期，各別process保存log。負例是Minisim範圍限制，沒有宣稱Icarus同樣拒絕；也不是任意外部VPI plugin／完整IEEE符合性驗證。

使用（WSL既有venv）：
- python tools/vpi_build.py ../basanos/hdl/tinyalu.sv --output build/my-alu
- python tests/run_alu_auto_manifest.py
- python tests/run_vpi_negative.py
- python tests/run_alu_fourstate.py

證據build/vpi-interface-negative、build/alu-vpi-auto-manifest、build/alu-fourstate。編譯與library版本另見runtime.json；原來源修改保留，kernel及VPI行為本輪未變，未推送／合併。下一步逐個遷移其他profile並加入失敗build／快取失效負例，不將此整理外推coverage/FSM/SVA完成。

## 2026-11-04：共用build故障注入

新增 minisim/tests/test_vpi_build_failures.py，對真正build()流程的編譯器呼叫作故障注入，使用臨時來源與binary fixture。三項測試3/3通過：編譯失敗保留原binary、編譯期間source被改動拒絕替換、成功後替換binary並可驗證provenance。這些是build流程測試，編譯器被替身控制，新增RTL模擬0；不冒充新G++／Icarus或VPI動態結果。初次測試的ExitStack匯入已修正後重跑成功。git diff --check通過，保留codex/minisim-four-state-mux及既有修改，未推送。

只讀確認portfolio容器exited；本輪主機可用1320292KiB約1.26GiB。初次自動權限審核逾時，允許的一次只讀重試已成功，沒有把逾時誤記為危險或使用者拒絕。未啟動整批回歸。

研究：英文arXiv與中文OSCC相關日期搜尋未取得可核實的新增24h發布，搜尋回傳含無關結果，未收錄為新聞。先前一手基線RTL-Interact https://arxiv.org/abs/2610.41225 發布/事件日期2026-10-30仍在7日窗，但未重新證實有新版本，不重計新發布。舊Verilator5.054研究與各coverage分母沿用，不宣稱本地升級完成。

下一P0（高效益／低成本／低風險）：將其他仍重複編譯的profile逐項遷移共用build並保留mutant與coverage設定；驗收需舊profile正常/負例及provenance正確。Portfolio仍依受控單VIP與checkpoint順序推進，OpenTitan/OpenCores尚未完成。後續可優先檢查lowering與來源快照的一致性窗口，不把SHA校驗稱hermetic build。


## 2026-11-04：coverage profile遷移共用入口

run_alu_vpi_coverage.py已改用tools/vpi_build.py、vpi_runtime.environment、vpi_results.check_results與provenance前後校驗，移除重複lowering、手工manifest、G++與env設定。重跑前清除指定舊XML/trace/line/toggle產物，避免錯用stale輸出；非空分母、非負整數count與closed一致性也檢查。原runner與完整artifact備份在build/vpi-coverage-migration/before及runner-before.py。

真實cocotb128筆scoreboard/XML PASS，line12/12、toggle76/78；完整bin keys與每項hit count均與遷移前完全一致，comparison.json已保存。這是單一128筆基線，不挪用先前跨run78/78當本輪結果；result[15] rise/fall兩個holes保留。kernel/host未改，未無故重跑其他profile。下一步依效益遷移剩餘需要維護的runner或整合共用run函式，不持續新增複製入口。

portfolio容器本輪確認exited；開始前host可用1024756KiB約1GiB，僅做單工小型Minisim host編譯，未恢復整批UVM，不動其他專案。新增真實128筆模擬，未推送／合併。

## 2026-11-04：lowering後來源變更缺口修正

已用故障注入重現共用build的時間窗口：lowering與manifest皆保存舊SHA，之後source在編譯前變更，原先before-input快照會捕捉新來源，卻沒有與lowering SHA交叉核對。新增test_changed_after_lowering_preserves_binary，修正前1/4失敗（RuntimeError未發生），不是僅依閱讀推測。

tools/vpi_build.py現在在編譯前input hash快照後，比較source SHA與model.source_sha256；不一致立即RTL changed after lowering。修正後4/4故障注入通過，包括compiler assert_not_called、舊binary保留、無成功provenance；原compiler失敗／編譯期間變更／成功路徑也通過。這些是mock compiler的build流程測試，真實RTL新增模擬0，不宣稱新的Icarus或VPI動態成果。

修改builder會使既有binary provenance對目前helper SHA失效，後續應重新用共用入口build，不能略過verify沿用舊artifact；歷史測試仍對原先版本有效。沒有聲稱hermetic build或解決所有外部dependency/來源讀取競態。WSL曾顯示systemd user session啟動警告，但測試本身退出0、4/4通過；未重啟WSL或其他專案。Portfolio容器只讀確認exited，未啟動整批回歸。未推送／合併。

下一步優先重新build需要執行的profile並保留來源一致性負例，再持續遷移舊runner。測試文件與來源均在既有codex分支保留。


## 2026-11-04：來源guard修正後動態驗收

已保留舊auto-manifest成果於build/vpi-source-guard-20261104/before-auto-manifest，再以修正後共用build重新生成TinyALU。真實cocotb128筆scoreboard/XML PASS；接續同新binary的VPI四態15/15與Icarus一致，X/Z input roundtrip通過。執行中既有provenance verify通過，另外核對目前binary、builder、RTL SHA與design.json/source SHA一致；summary在build/vpi-source-guard-20261104。

這輪因前一版builder SHA失效而必要重建，沒有跳過verify沿用舊binary；修正前紀錄保持歷史有效。新增正常128筆與四態15筆實際模擬，不宣稱本輪重跑其他core/pyuvm/coverage或portfolio。來源guard四項故障注入沿用上一輪結果，沒有無故重跑。開始前可用1121140KiB約1.07GiB，單工小型host編譯成功，portfolio容器仍exited，未動其他專案。未推送／合併。

下一步可依效益把共用run與其他profile整合，或在資源允許時受控續跑VIP checkpoint；OpenTitan/OpenCores順序仍保留。有限flat VPI、普通case與coverage分母限制不變。


## 2026-11-04：原pyuvm profile自行共用build

run_basanos_pyuvm_vpi.py已遷移共用build/runtime/XML/provenance入口，直接從原TinyALU RTL建立本profile binary，不再隱含依賴build/alu-vpi/simulator。build納入原basanos/testbench.py與runner SHA，執行前後verify；刪除指定舊XML/TX避免stale假通過。舊runner與完整成果保存build/pyuvm-shared-build-migration/before。

真實原suite500筆scoreboard、0錯、XML PASS，恰500筆sample交易。新TX與遷移前及保存Icarus seed0 baseline逐筆內容/時間完全相同，三檔SHA亦一致；comparison.json已保存。本輪沒有重新跑Icarus，沒有把既有比較冒稱新模擬。開始前host可用2521056KiB約2.40GiB，單工小型Minisim build成功，portfolio仍exited且未啟動整批UVM。新增真實Minisim pyuvm500筆，未推送／合併。

下一步依相同原則處理需要維護的負例profile／共用run函式，保留DUT mutant與交易檔mutant區別；portfolio與外部IP固定順序不變。


## 2026-11-04：原pyuvm負例profile共用build

run_basanos_pyuvm_mutant.py已遷移共用build/runtime/XML/provenance，從原RTL產生獨立mutant.sv（僅一處a*b改a+b），不再依賴舊alu-vpi-mutant binary。mutation.json記原/錯誤RTL各自SHA，build來源是mutant本身，並校驗原testbench／runner／正確RTL與mutation描述。舊runner與完整成果保存build/pyuvm-shared-mutant-migration/before。

原suite真實500筆完整檢查、97錯，XML預期scoreboard failure；新負例TX與舊負例逐筆完全一致。另與保存正確Icarus基線比對500筆刺激與時間完全相同，獨立ALU模型計算亦得到97個錯誤結果，原TinyALU SHA未改；comparison.json保存。沒有新跑Icarus mutant。此負例仍明確不算正常DUT PASS。

開始前host可用750272KiB約733MiB，本輪僅單工小型host build並設每process虛擬記憶體512MiB上限，整合成功；未恢復整批portfolio，不動其他專案。新增真實負例500筆，未推送／合併。下一步依效益整合剩餘run流程或回到受控VIP checkpoint試跑；不擴大VPI／coverage／語言支援宣告。


## 2026-11-04 23:32 Taipei：共用build逾時與生成標頭負例

新增test_compiler_timeout_preserves_binary與test_changed_header_preserves_binary；在真實build流程以mock compiler注入TimeoutExpired或改寫design.hpp。6/6單元測試通過，兩新負例均拒絕發布新binary、保留原binary且不產生成功provenance。只修改tests/test_vpi_build_failures.py，builder／host未改，沒有必要重跑RTL；本輪新增RTL／Icarus模擬0，不把mock測試當動態VPI成果。

分支codex/minisim-four-state-mux，既有修改保留；host可用929868KiB約908MiB，portfolio只讀確認exited，未啟動大型編譯。下一步P0受控單VIP checkpoint驗證（效益高、成本中、資源風險中，驗收單項CSV原子落盤與狀態分類），P1按需集中run流程（成本低，驗收XML與provenance負例）。未推送／合併。

研究增量：英文arXiv日期與中文OSCC搜尋未取得可核實24h新增AI EDA一手發布；搜尋包含聚合頁與無關結果，不以crawler日期當事件日期。官方基線 https://ieda.oscc.cc/ 及其AiEDA-2.0論文 https://ieda.oscc.cc/res/papers/25-ISEDA25__AiEDA_2_0.pdf 屬既有資料，未計新發布，沒有新增本地工具能力宣稱。


## 2026-11-05 00:02 Taipei：CSV結尾原子寫入修正

檢查受控單VIP入口時發現V1 patch僅逐項checkpoint原子寫入，main結尾仍open(REPORT,w)截斷重寫；若此階段中斷，可能破壞先前CSV。Helios scripts/patch_vip_coverage_checkpoint.py升級V2，結尾亦呼叫checkpoint_rows，支援既有V1升級。新增scripts/test_vip_checkpoint_finalization.py，首次/升級內容一致、idempotence、結尾呼叫檢查、replace故障保留舊CSV並恢復合併，4/4通過；git diff --check通過。

本輪沒有啟動容器或RTL模擬，沒有宣稱實際VIP落盤驗收。V2尚未copy到停止容器；容器仍exited、內部仍為先前V1，下次受控執行前必須升級並核對SHA。host可用1686208KiB約1.61GiB，不以container上限當最低需求。保留全部部分coverage／timeout分類，未推送。

P0下一步：部署V2至private snapshot並回讀校驗，安全建立受控單VIP入口，驗收真實CSV及程序狀態；效益高、成本中、資源風險中。Minisim共用build與6項故障注入沿用前輪有效證據，無變更未重跑；本輪新增模擬0。


## 2026-11-05 00:32 Taipei：停止容器V2部署驗證

將目前container private snapshot的V1 coverage.py先取出保存，再以patched()升級V2，copy回同一路徑並再次讀回；原上游repo未改。證據build/vip-coverage-checkpoint/deploy-v2-20261105/deployment.json及before.py/coverage.py/readback.py。回讀SHA與部署檔完全相同fbee3655c9f559d6efdebe21b389304244a2d18c4be8a6dd3baf9e909dd60a49，語法compile通過且V2 marker存在；前SHA32e6cbf4c53a5e6f9e1811400b6619f09adc37d6498f0d865e4af81a82e3de2c。

容器前後皆exited，沒有start controller或重跑先前PASS；新增RTL／VIP模擬0。這是部署內容校驗，不是真實單VIP CSV落盤驗收，後者仍待受控入口。沿用前輪4/4測試，無程式變更不重跑；Minisim build6/6與Icarus既有成果未擴大宣告。下一步建立安全單VIP命令入口、檢查資源並保留timeout／partial分類。未push／merge／啟動付費API。


## 2026-11-05 14:32 Taipei：Minisim預期失敗XML結構修正

回到Minisim共用vpi_results.py，重現錯誤驗收：testsuite下孤立failure或testcase/attachment/failure只要文字符合mismatches就會被接受為expected mutant failure，未證明真實testcase失敗。新增orphan/nested兩個負例，修正前6項中2項失敗（ValueError未發生），有實際重現。

現在failure必須直接隸屬已計數testcase，否則拒絕。修正後6/6通過，並重新解析保存的真實basanos-pyuvm-vpi正常XML與basanos-pyuvm-mutant（mismatches!）XML，2/2仍符合新規則；結果保存build/vpi-results-structure-20261105/saved-xml-validation.json。這是XML驗收／歷史檔重驗，新增RTL模擬0，沒有重跑500筆或Icarus。

檔案tools/vpi_results.py、tests/test_vpi_results.py及本docs；既有codex/minisim-four-state-mux與使用者修改保留，diff check通過。未改kernel/host/buildhelper，歷史binary不因此新增hermetic宣告。下一步檢查其他結果結構邊界及用新驗收器跑需要的新profile；Portfolio仍依固定順序續跑，GPIO UVM失敗保留。未推送。


## 2026-11-05 15:02 Taipei：XML宣告計數一致性修正

Minisim vpi_results.py原先僅數實際節點，testsuite宣告failures/errors/skipped1卻缺節點、宣告tests0卻有testcase、或tests非數字，均會被接受正常PASS。新增5個subTest，修正前全5錯誤接受已重現。

現在逐個testsuite/testsuites核對有提供的tests/failures/errors/skipped非負十進整數與各子樹實際節點數，矛盾／非法宣告拒絕；沒有統計屬性的舊fixture仍相容，不強迫補欄位。八個單元方法8/8通過（含5個新負例），保存的真實pyuvm正常與97mismatch mutant XML2/2接受；declared-counts-validation.json保存。不是重新模擬500筆，新增RTL/Icarus模擬0。

tools/vpi_results.py、tests/test_vpi_results.py及docs於既有codex分支保留；diff check通過，kernel/host/build未改。下一步依效益處理真實新profile或未完成VIP，避免只擴充無實據驗收條件；GPIO UVM與portfolio未完項保留。未推送。


## 2026-11-05 15:32 Taipei：64位元真實VPI/Icarus差異測試

新增tests/vpi_wide.sv單一flat64bit clocked register、cocotb_vpi_wide.py與run_vpi_wide.py；runner沿用tools/vpi_build/runtime/results/provenance，不另複製build流程。來源、cocotb、runner與向量JSON均納入provenance，模擬前後verify；使用新XML結構/計數驗收器。

真正cocotb VPI10筆XML PASS且input/output逐筆與預期/Icarus完全一致：0、1、bit31、bit32、bit32+bit0、bit63、全1、全X、全Z、10xz混合64bits。獨立Icarus compile/run同原RTL，reference.log、trace/vectors、XML/build/runtime/provenance/summary保存build/vpi-wide-64。沿WSL既有venv，單工且process虛擬記憶體512MiB上限；新增真實Minisim10筆及Icarus10筆對照，未重跑TinyALU500或既有core。

範圍僅flat64bit暫存器、binary-string VPI寫讀與posedge傳遞，未宣稱所有vector/int/real格式、64bit arithmetic/fullIEEE或一般hierarchy。kernel/host未改，沒有捏造完整SVA/FSM closure。下一步依實際需求檢查其他VPI格式邊界或續未完成VIP；Portfolio GPIO UVM失敗保留。未推送。


## 2026-11-05 16:02 Taipei：VPI null vector崩潰修正

讀取vector word路徑時發現未檢查value.vector，新增既有negative harness null-vector案例，真正host呼叫修正前returncode -11 SIGSEGV、無錯誤訊息；null-vector-before.json保存，不是僅靜態推測。vpi_generic_main.cpp現於讀word前明確拒絕null vector value。

修正後共用build實際八項介面檢查8/8符合預期（7明確拒絕＋missing name回null），summary與每項log保存build/vpi-interface-negative。host改動會讓舊host provenance失效，因此必要重建run_vpi_wide profile，真實cocotb64位元10/10與新Icarus輸出一致、XML/provenance驗收通過。限制process虛擬記憶體384MiB，未重跑TinyALU500/core20。

檔案tests/vpi_generic_main.cpp、vpi_negative_main.cpp、run_vpi_negative.py；diff check通過。這只修null vector payload，不宣稱任意外部pointer皆安全或完整IEEE符合；原本第二word vector專用直接API對照仍待進一步測試，既有64位元成果為binary-string介面。新增VPI API負例1與正常64bit重驗10筆，Portfolio停止狀態與GPIO失敗保留。未推送。


## 2026-11-05 16:32 Taipei：直接64位元vector兩word實測

新增tests/vpi_vector_main.cpp直接呼叫真實host vpi_put_value/vpi_get_value格式vpiVectorVal，使用兩個32bit aval/bval word，覆蓋0/全1/高bit63/兩word混合X-Z/全X/全Z共6案例；先核對兩word各aval/bval逐字相等，再讀binary string與獨立Python編碼預期一致。6/6通過，沒有host/kernel改動。

run_vpi_vector.py明確verify並沿用共用build先前產生的vpi-wide-64 design/manifest/core來源，僅編譯新直接API harness，不複製lowering；harness成功後另保存binary/input SHA並執行前後verify。Icarus實跑六個相同64bit四態字面值，輸出binary逐筆一致。此對照是Icarus RTL編碼reference，不是將同一VPI plugin載入Icarus；也不等於新cocotb scheduler案例。證據build/vpi-vector-64 native/reference logs、headercases、summary、provenance。

開始前host可用752804KiB約735MiB，process虛擬記憶體384MiB限制，新API harness6筆及Icarus reference6筆完成；未重跑已過10筆cocotb/500筆pyuvm。direct input向量格式範圍已補驗，任意width/hierarchy/delayed/force/fullIEEE仍未支援。下一步優先續未完成VIP或泛化可重用API測試，不不斷新增複製runner。未推送。


## 2026-11-05 21:02 Taipei：module handle讀值斷言修正

延續VPI handle類型檢查，新增既有harness module-read。修正前真實呼叫不是單純讀出0：stderr觸發std::vector<Sig>::operator[]索引斷言，module-read-before.log保存，negative runner失敗。原程式除Constant以外一律S.get(h->signal)，module handle不是合法signal id。

vpi_generic_main.cpp現在read只接受VH::Signal/VH::Constant，其餘明確read requires signal or constant並終止adapter，避免進入核心非法索引。修正後九項API情境9/9符合預期（8拒絕+missing-name null），host變更後必要重建64位元profile，真實cocotb10/10仍與Icarus一致，XML/provenance驗收通過。process限制384MiB，未重跑500筆pyuvm/core。

檔案host、negative harness/runner；diff check通過。未宣稱所有handle lifetime/任意pointer或完整IEEE符合。先前vector6筆對應舊host歷史證據仍保留，未冒稱本輪重驗。Portfolio GPIO/TDM失敗及其餘未完成項保留。下一步續未測功能lane或有實際用途的API整合，未推送。


## 2026-11-06 12:43 Taipei：RTL property結束條件評測缺口

低資源工作僅重新解析既有SVA raw evidence：clean六個cover hit與summary一致、no-start六項一致，mutant log含指定ap_run_done；證據build/sva-evidence-audit-20261106.json保存四份原檔SHA。這是歷史資料稽核，新增模擬0，不增加原有測試數。

具體缺口：tests/rtl/coverage_harness.cpp目前80 cycles後直接final；不能由此宣稱RTL property pending obligation為0。src/coverage.hpp C++ ClockedProperty.has_pending驗收屬另一介面，不外推真正RTL SVA。P1下一評測須在固定刺激後停止新antecedent、至少完成已知一拍obligation的drain，並建立末拍觸發、drain前失敗／drain後錯誤consequent被拒絕的正負例；若加顯式shadow monitor須標示其獨立模型身分，不稱Verilator內部pending讀值。驗收保存sample時序、trigger/resolution計數、RTL assertion結果，未實作／未PASS前不提高closure聲明。

host可用385472KiB約376MiB，Portfolio exited、其他buildkit未動，CSE未attempt；仍10已量測、4partial、116無紀錄、full0。優先P0資源可行時CSE單項，P1再處理上述Minisim/參考property評測，避免重跑既有PASS。未推送。


## 2026-11-07 00:14 Taipei：每日基線與property評測優先項

已更新Helios docs/research/2026-11-07.md。官方Verilator5.054（11/01）列non-overlapped implication修正，僅上游宣告；本地5.032／Portfolio5.052未升級。P1固定RTL/刺激核對末拍trigger、下一拍consequent、reset disable及drain，保留錯誤DUT與vacuity/pending；不將原生C++ property API當RTL SVA parser。四份收斂報告SHA吻合，新增模擬0；P0持續可達coverage holes收斂，未推送。


### 2026-11-07 09:45 Taipei — PR #16 temporal-obligation fixture design

Inspected the actual RTL implication and 80-cycle harness. Independent two-state schedule checks cover five cases: last-cycle pending, one-edge drain, wrong consequent, no antecedent, and reset cancellation. All five expected model outcomes were checked; this is schedule-design validation only, not new RTL simulation or formal proof. Starting at cycle 2 yields RUN/cnt=3 at sampled cycle 6; ending there leaves an obligation, cycle 7 resolves it. Reset cancellation must not manufacture a nonvacuous resolution. Source SHA and complete sampled timelines are saved in build/rtl-obligation-plan-20261107/schedule-cases.json. Next implementation should extend the existing harness (no duplicate runner), sample before rising-edge eval, and execute actual assertion clean/mutant tests. Draft tracker: https://github.com/tonythetiger168/minisim/pull/16 .


### 2026-11-07 10:15 Taipei — actual RTL end-obligation regression

Extended existing coverage_harness.cpp and run_verilator_coverage.py, without a duplicate runner. Eight end-case executions (clean/mutant x drained/undrained/no-start/reset) matched expected outcomes. Clean drained resolves one trigger; undrained returns 2 with pending=1; no-start and reset-cancelled return 3 for missing nonvacuous resolution. Drained BUG mutant is rejected by the actual RTL ap_run_done assertion. Shadow pending is an independent fixture monitor, not Verilator internal state. Existing six state/arc cover checks, no-start zero-hit check, and ordinary assertion-mutant rejection also passed. Raw logs, coverage and source/log SHA are in build/coverage/sva-end-obligation-20261107/end-obligations.json. This is Verilator reference RTL validation, not a native Minisim SVA parser or full closure. PR #16 follow-up implementation remains to publish/review.


### 2026-11-07 10:45 Taipei — strict obligation-result acceptance

Existing runner now validates exact trigger/resolution/cancellation/pending/failure counts and the complete ordered sample sequence, rather than only matching an exit code and OBLIGATION marker. All eight saved real RTL execution logs accepted; six corrupt evidence variants rejected (empty, missing resolution, pending obligation, duplicate summary, duplicate sample cycle, wrong exit). No RTL simulation repeated. Evidence: build/coverage/sva-end-obligation-20261107/validator-audit.json. Actual mutant signal code is not hardcoded for acceptance: nonzero plus the intended ap_run_done assertion and complete pre-abort samples are required. Git diff check passed. PR publication remains pending the previously requested authorization; no external write retried.


### 2026-11-07 11:45 Taipei — legacy TinyALU runner migration

Migrated tests/run_alu_vpi.py from its duplicated compiler command, hand-written port manifest, environment and weak XML check to existing vpi_build / vpi_runtime / vpi_provenance / vpi_results helpers. Added --output so fresh evidence does not overwrite historical runs. Real Minisim flat VPI cocotb execution at seed 168 passed 128 scoreboard samples and one nonempty XML testcase; input/binary provenance verified before and after simulation. Evidence: build/alu-vpi-shared-20261107 (build log, source hashes, runtime identity, trace, XML and summary). No new runner was created and no coverage closure claim added. The old generated-header mutant runner and specialized vector/negative harnesses still require separate migration decisions. PR #16 external update remains blocked pending prior requested publication authorization.


### 2026-11-07 12:15 Taipei — shared-build DUT mutant acceptance

Migrated run_alu_vpi_mutant.py to shared build/runtime/provenance/XML helpers. A private RTL copy changes exactly one MUL expression from multiplication to addition; Basanos source unchanged. This differs explicitly from historical generated-header Op mutation evidence. Real cocotb rejected the DUT mutant with intended ALU mismatch sample in exactly one XML failure; compiler/process failure cannot satisfy acceptance. Provenance verified before and after simulation. Evidence: build/alu-vpi-mutant-shared-20261107. No mutant coverage merged. Both old TinyALU entry points now use shared lifecycle helpers. No full VPI/language/coverage closure claim.


### 2026-11-07 15:15 Taipei — runtime metadata failure before binary publication

Found vpi_build replaced simulator before runtime identity discovery; a discovery exception could leave a new binary with incomplete metadata. Runtime identity is now resolved before replacement. Seven build failure/success regression methods passed, including a new post-compile identity-failure case that preserves the previous binary and writes no provenance. Evidence: build/vpi-runtime-metadata-20261107/result.json. This does not claim an atomic transaction across all binary/metadata files; post-publication filesystem failures remain a separate limitation. Existing source-hash guards and no-implicit-cache policy retained. No real simulator run repeated for this failure-order-only change.


### 2026-11-07 18:15 Taipei — pyslang bitwise OR

Added BinaryOr to the restricted pyslang binary-operator mapping, using the existing native Op::Or implementation. Actual two-bit exhaustive four-state test (256 pairs over 0/1/X/Z) matches Icarus for every output. A logical-OR substitution mutant disagrees on 240 pairs and is rejected. Evidence: build/frontend-bitor-20261107/result.json, RTL/generated header/native and Icarus logs. Scope remains flat unsigned 1..64-bit frontend; dynamic evidence is specifically two-bit OR, not complete SV support or portfolio acceptance. Build/evaluation script is a local evidence artifact; no new shared runner framework introduced. Existing rejection boundaries remain.


### 2026-11-07 19:15 Taipei — persistent OR regression entry

Moved the proven OR oracle/stimulus evaluation into existing tests/run_pyslang_frontend.py with --bitwise-or-only; no separate runner added. This closes the gap where reproduction depended on an ignored build artifact. Added exact 256-row requirement for mutant output before comparing mismatches. Real execution through the maintained entry again matched Icarus on 256 four-state pairs and rejected logical OR on 240; this validates entry integration, not new semantic coverage. Reproduce: python tests/run_pyslang_frontend.py --bitwise-or-only --output build/frontend-bitor-entry-20261107 (WSL venv). Historical evidence preserved.


### 2026-11-07 20:45 Taipei — OR regression failure cleanup

Replaced OR-path assert acceptance checks with explicit RuntimeError checks, preserving validation under Python optimization. Mutant header restoration is now in finally. Fault injection of mutant compilation failure verified error propagation, restoration of the clean header, and absence of a new PASS report. No simulator rerun; existing semantic evidence remains unchanged. Evidence: build/frontend-bitor-failure-20261107/result.json. This fixes failure cleanup only, not full artifact transactionality or signed/hermetic provenance.


### 2026-11-07 21:15 Taipei — OR output reuse rejection

The OR-only regression now requires a fresh output directory before touching any artifact. This prevents a failed rerun from leaving a previous PASS report looking current. A pre-existing report/header fixture was rejected with FileExistsError and both files remained byte-identical. No simulation repeated. Evidence: build/frontend-bitor-output-guard-20261107/result.json. Existing counter-mode output behavior is unchanged; apply this policy incrementally where appropriate, not a blanket claim for all runners.


### 2026-11-08 — runtime discovery source consistency

The shared VPI builder now rechecks captured input hashes after runtime identity discovery and before replacing the simulator. Previously a source change during that discovery could escape the earlier post-compile check and be recorded against an older compiled binary. Eight targeted build regression methods pass, including an injected source change during discovery that preserves the previous binary and writes no provenance. Evidence: build/vpi-runtime-source-guard-20261108/result.json. No simulation repeated. This narrows a consistency gap; it is not hermetic execution or an atomic multi-file transaction, and concurrent changes after the final check remain possible. Next priority: use fresh evidence outputs while migrating remaining specialized entry points only where shared lifecycle helpers apply.


### 2026-11-08 — supplement runner migration priority

Review found run_alu_holes_vpi.py and run_toggle_holes_vpi.py still execute pre-existing binaries without the shared provenance verifier. A final binary SHA alone does not prove equivalence to the baseline that produced coverage. Prioritize the functional supplement migration (small cost/high reproducibility benefit), then toggle merge identity (medium cost; preserve baseline binary). Reuse existing build/runtime/results helpers and entry points; require fresh outputs, pre/post verification, same fixed nonempty denominators and actual directed samples. Review/source hashes and detailed acceptance: build/vpi-supplement-migration-review-20261108.json. No simulator run or revised closure claim this round; historical evidence remains untouched. Docker unavailability does not block this WSL work.


### 2026-11-08 — functional supplement shared lifecycle accepted

Migrated existing tests/run_alu_holes_vpi.py to shared build/runtime/provenance/XML helpers. It builds the unchanged Basanos TinyALU with the directed test, fixed-bin analyzer, baseline TX and plan included in provenance, verified before/after simulation. Fresh --output is mandatory in practice (existing directory rejected); --baseline/--plan enable explicit inputs. Actual cocotb execution produced31 valid directed samples: historical500 samples21/55 plus new31 gives531 samples55/55. This reproduces known functional closure through the maintained shared lifecycle, not new bin coverage or a single global timeline. Stale plan, altered stimuli and existing-output negatives all rejected before build; prior output preserved. Evidence: build/alu-holes-shared-20261108 (summary, XML, logs, provenance, guard-results.json). Historical artifacts untouched. Next: migrate toggle supplement while preserving exact baseline source/binary/bin identity; do not rebuild it silently.


### 2026-11-08 — toggle supplement identity migration accepted

Migrated existing run_toggle_holes_vpi.py to shared runtime/provenance/XML acceptance, explicit --baseline/--output and fresh output directories. Historical baseline was correctly rejected as stale (vpi_generic_main.cpp changed); it was not rebuilt or relabelled. Added --output and a binary/toggle/provenance hash record to the existing coverage runner, then created a separate128-sample baseline: line12/12, toggle76/78. Two real cocotb directed samples on that same verified binary close the exact78-bin union to78/78. Baseline and supplement verified before/after execution; supplement test hashes retained. Three pre-simulation negatives passed: historical stale baseline, modified toggle file, existing output (prior evidence preserved). Evidence: build/alu-toggle-baseline-shared-20261108 and build/alu-toggle-supplement-shared-20261108 (summary/XML/provenance/guard-results.json). Revalidation of maintained entry integration, not new semantic coverage or a single global timeline. Content hashes are local consistency evidence, not signed/hermetic provenance or proof that arbitrary fabricated reports are genuine.


### 2026-11-09 — next frontend operator evaluation

The native expression evaluator already implements bitwise complement with `Op::Not`, including conversion of Z to X. The pyslang frontend currently accepts only `LogicalNot` for runtime unary expressions. Evaluate a narrow `BitwiseNot` mapping next. This has low implementation cost and improves reuse of existing native semantics. The main regression risk is operand sizing when the assignment destination differs from the source width.

Extend an existing differential entry with a dedicated mode. Require 256 exhaustive four-state four-bit inputs, directed width expansion and truncation cases, and a 64-bit boundary case. Compare ordered rows with Icarus and a per-bit oracle. Require the logical-not substitution to complete with the same row count and disagree on known input cases. Keep signed and two-state runtime expression rejection checks. Record a fresh output and source hashes. A change to the lowerer will invalidate older reusable VPI build records, so preserve those artifacts as historical measurements. This entry records evaluation design. No simulator ran in this round.


### 2026-11-09 — frontend bitwise complement verified

`tools/pyslang_lower.py` lowers runtime unsigned bitwise complement to existing native `Op::Not`. The dedicated mode in `tests/test_lognot_wide.py` passed 256 exhaustive four-state four-bit inputs, with four outputs per sample covering equal width, expansion to eight bits, truncation to two bits and five rotating 64-bit patterns. All ordered rows agree with Icarus and the independent per-bit oracle. Replacing the four bitwise complements with logical negation produced all 256 rows and disagreed on each. Seven existing frontend unit tests passed, including signed and two-state rejection. Evidence in build/frontend-bitnot-20261109, including generated RTL, traces and source SHA. Reproduce with `python tests/test_lognot_wide.py --bitwise-not --output build/frontend-bitnot-fresh` in the WSL venv. Scope is unsigned flat RTL. Older VPI build records that pin the previous lowerer remain historical evidence and require a fresh build before reuse.


### 2026-11-09 — frontend implementation PR dependency review

The local lowerer emits `Resize`, `Instr.events` and `Instr.line_bin`. The differential harness includes the native runtime under `MINISIM_NO_DEMO`. The current runtime also includes `coverage.hpp`, which is still untracked alongside the lowerer and test. A frontend implementation PR needs the corresponding native interfaces and their execution paths. Copying only the Python files would leave unresolved C++ dependencies.

Prepare an isolated candidate against the target revision and select the required native hunks before adding the lowerer, frontend fixtures and reproducibility instructions. Review the candidate for unrelated VPI callbacks and coverage work. Acceptance requires a clean-checkout build, the existing 256-row complement oracle and mutation check, and the frontend rejection tests. Link the implementation PR to tracking PR #16 once publication is authorized. This review inspected source dependencies. No simulation or remote write was performed.


### 2026-11-09 — implementation PR scope after checking upstream

Fetched `origin/main` at `56f046a4c9f5844722ccf8d88c736a716758f70d`. The local Codex work starts from `fe6551c`. Current upstream already lowers `UnaryOperator::BitwiseNot` to `Op::Not` in `src/lower.hpp` and implements native evaluation in `src/expr.hpp`. This supersedes the initial recommendation to open a PR for bitwise-complement implementation. The local pyslang result is still evidence for that separate path. Source inspection alone leaves current upstream behavior at the tested width and X/Z boundaries unmeasured.

Next, inspect existing upstream fixture coverage and run the four-state complement case against the native CLI on an isolated checkout of the fetched revision. If it exposes a regression, isolate the fix and its fixture. If behavior agrees and the fixture adds coverage, consider a test-only contribution. Evaluate the distinct value of the Python/VPI bridge separately before proposing any frontend port. No simulation ran in this round. The existing worktree was preserved.


### 2026-11-09 — upstream native bitwise complement comparison

Built the native `msim` CLI from isolated `codex/upstream-bitnot-evaluation-20261109` at `56f046a4c9f5844722ccf8d88c736a716758f70d`. Used one G++ `-O0` compile with the existing slang v12.1 static library. The native CLI executed the saved DUT and testbench for 256 four-state input rows. All four outputs per row matched the saved Icarus reference. The fixture covers equal width, widening, truncation and rotating 64-bit boundary patterns. Build command, output, source hashes and result are under `../minisim-upstream-bitnot-20261109/build/bitnot-audit/`. The reference run was reused. This measures the upstream native CLI at the pinned revision and preserves the separate Python frontend evidence.

The behavior is already implemented and passes the targeted comparison. Keep PR #16 as tracking. A test-only follow-up is useful if it fills a gap in the upstream spec suite. Existing `11.4-wide.sv` exercises wide complement. Compare its X/Z and context sizing cases before adding a fixture. No simulator process remains from this check.


### 2026-11-09 — test-only upstream candidate ready for review

The upstream wide fixture checks complement of a known 128-bit value. Added `tests/spec/11.4.8-bitwise-not.sv` in isolated local commit `414f3994ddb2dcac800b82559ee2e8b438cf314c`, based on `56f046a`. The three output rows check X/Z complement at equal width, unsigned assignment widening, truncation and an unknown bit above the 64-bit limb boundary. The existing `run_spec.cmake` driver passed against both the pinned native CLI and Icarus. The candidate changes one fixture. No runtime code is included. This is a reviewable test-only follow-up to PR #16. Publication awaits an explicit request.


The same spec fixture passed both generated C++ configurations on the pinned upstream CLI. Default `-compile` and `-compile -no-2state` each built a binary, completed three expected rows and reported no interpreter fallback. Evidence includes both binary hashes and logs in `../minisim-upstream-bitnot-20261109/build/bitnot-audit/compiled-spec-result.json`. The default flag selection does not show that a two-state optimization was exercised by this X/Z fixture. The separate logical-negation substitution produced three wrong rows, and the upstream spec driver rejected the mismatch. See `spec-mutant-result.json` in the same evidence directory.


### 2026-11-09 — compiled complement runtime guard

An evidence-only fixture fed four inputs to one 4-to-8-bit complement assignment, alternating known values and X/Z. Both the normal compiled binary and a generated C++ copy with a guard probe produced four expected rows. The probe preserved the guard predicate and observed fast, slow, fast, slow. Evidence and logs are under `../minisim-upstream-bitnot-20261109/build/bitnot-audit/runtime-guard/`. This measures one expression at the pinned upstream revision. The published candidate scope remains a single spec fixture on the local branch.


### 2026-11-09 — upstream native CLI runs Basanos TinyALU

On pinned upstream `56f046a`, the saved 535-case TinyALU bench ran on the native CLI with zero row mismatches against the previously checked Icarus reference. First, the CLI rejected the two separate source files because the bench specifies a timescale and the DUT has none. The accepted run used one compilation unit with the bench first so its timescale applies to the unchanged DUT bytes. The separate-file diagnostic, accepted logs and hashes are in `../minisim-upstream-bitnot-20261109/build/tinyalu-upstream-20261109/`. This establishes direct RTL simulation in the single-compilation-unit configuration. The separate local Python/VPI evidence still covers cocotb and pyuvm integration. No portfolio outcome changes from this isolated comparison.


The upstream CLI comparison also rejects a single-site TinyALU RTL mutation. The MUL assignment uses addition in a private compilation unit. The mutant completed all 535 rows with normal exit and disagreed with the clean Icarus reference on 62 rows. Every mutant row matched the separately calculated mutated behavior, including retained results on idle cycles. The changed row IDs and hashes are in `../minisim-upstream-bitnot-20261109/build/tinyalu-upstream-20261109/mul-as-add/result.json`. This is direct RTL negative evidence for the native CLI. The separate pyuvm mutant has its own traffic and denominator.


### 2026-11-09 — upstream VPI port boundary

A G++ syntax-only probe against pinned upstream `56f046a` used the existing local VPI host and saved TinyALU generated header. It identified missing timer, edge, read/write and read-only callback hooks, cancellation, advance, and source coverage members. The upstream `resize` signature additionally requires sign-fill and result-signedness arguments. The compile exited 1 before simulation. Exact diagnostics and command are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-interface-audit/`.

Prioritize a narrow scheduler seam before VPI integration. Its first acceptance case should cover a timer-only slot, cancellation, one value-change callback, read/write changes that settle through NBA, and read-only after quiescence. Use the upstream event-driven single-thread path first. Then adapt the existing host to the upstream value representation and use one real cocotb counter with 96 samples plus the expected mutant rejection. Defer source coverage, compiled scheduling, and wide VPI vectors until those can be measured independently. This isolates the main behavioral risk at the scheduler boundary and keeps the validated local bridge available.


### 2026-11-09 — upstream-based VPI counter prototype

A private copy of upstream `56f046a` now runs the existing cocotb counter through the adapted local VPI host. The probe adds single-thread timer, value-change, read/write and read-only dispatch to the upstream interpreter, preserves the existing local unsigned frontend for the counter, and adapts the upstream `resize` signature. Source coverage is omitted from this experiment. The unmodified counter completed 96 scoreboard samples with a nonempty passing cocotb XML result. Source and binary hashes, generated headers, compile command and logs are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-counter-20261109/`.

This is a private single-thread integration prototype. Next require the increment-by-two RTL mutant to fail with the named counter scoreboard error. Then separate reusable callback code from harness adaptation and test budget, cancellation, reentry and write rejection on the integrated path. Pyuvm, wide values, parallel and compiled scheduling are separate scopes.
