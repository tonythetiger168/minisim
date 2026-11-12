# pyslang → Minisim IR 前端

2026-11-01 13:01（台北時間排程觸發）跟進：新增獨立、有限子集的 Python 前端 `tools/pyslang_lower.py`。這條路徑已實際編譯執行，不再只使用手工 counter IR；原有 C++ `src/lower.hpp` 仍未編譯驗收，兩者不可混稱。

## 支援與拒絕政策

使用 pyslang 12.1.0 的 elaborated AST，不以文字規則猜測 RTL 語意。目前支援一個 flat top、1..64-bit unsigned 四態訊號、單邊緣 always／always_ff、begin/end、if/else、whole-variable NBA，以及已知整數常數與 unsigned +／-／==／!=。位寬 conversion 產生明確 Resize IR，保留 X/Z 並正確 zero-extend／truncate。

未知語法一律 Unsupported／非零退出，不默默跳過。尚不支援 SVA、階層、multi-event／async reset、signed runtime、two-state signals/casts、宣告初始值、partial NBA、blocking assignments、case、loop、任意函式、macro/include 來源映射、完整 SystemVerilog expression sizing/語法。已由 slang 計算的已知 integral constant 可折疊；signed runtime expression 不會被當作 unsigned 執行。

此支援子集比原有 C++ lower.hpp 的預期功能小，但已具備可重跑的端到端證據。不代表完整 RTL compiler 或 SVA engine 已完成。

## 來源映射與產物

- `design.json`：來源檔、SHA-256、pyslang 版本、訊號位寬、程序 IR 與 source_map。
- `design.hpp`：從該 IR 產生的 C++ constructor，可在實際 Minisim kernel 上建立程序；沒有重新手寫 DUT 算法。
- line bin 由 SourceManager 的真實檔名與行號產生，key 含 source SHA 前綴。條件／賦值映射到 executable source lines；控制流程輔助 jump 不另冒充 RTL 行。
- 未執行但已映射的指令保留為 0。無 mapped line 不可宣稱 100%。不同來源 SHA 的分母不直接混合。

重跑（在 WSL repo 根目錄，使用既有隔離環境）：

```sh
~/.venvs/ai-eda/bin/python tools/pyslang_lower.py examples/counter.sv --output build/frontend-probe
~/.venvs/ai-eda/bin/python -m unittest discover -s tests -p test_frontend.py -v
~/.venvs/ai-eda/bin/python tests/run_pyslang_frontend.py
```

## 實際驗收

- 5 個 Python tests 通過，包含來源映射、改名、begin/end＋無 else、明確 sizing，以及 16 類拒絕案例。
- 三個 RTL 版本：原始 4-bit、8-bit、increment=2，各 96 筆取樣，合計 **288 筆逐筆匹配真實 Icarus**。
- 刺激包含 reset、連續 reset、計數、4-bit wrap，以及 rst 為 X/Z 時的條件行為。不是任意四態設計的完整符合性驗證。
- 8-bit 與 increment=2 的 trace 都與 clean 不同，確認前端真的遵循 RTL 變更。increment=2 在這項測試中應匹配它自己的 Icarus RTL，不是被故意判錯；與前一輪 golden scoreboard 拒絕 mutant 的測試目的不同。
- 原始 counter 的來源行號為 4／5，兩個 bin 均命中。變體亦保留各自 SHA、source map、trace 與 line.json。
- 新增 Resize 的 X/Z zero-extension／truncation 檢查；Release 重新建置後 CTest **12/12 通過**。

輸出在 `build/coverage/frontend/`，含各 case 的 input.sv、design.json、design.hpp、trace.txt、line.json 及整體 summary.json。來源原檔未修改；變體在隔離的輸出路徑生成。所有內容仍在本地 codex/minisim-four-state-mux 分支，未推送或合併。

## 下一步

1. 在 AST→IR 層辨識 SVA 節點，定義 clock／sampling／disable／單步 implication 子集；尚未支援的 temporal operator 必須拒絕。
2. 將既有 clocked-property monitor 連接到真正降低後的 DUT，並對照 Verilator 的 assert／cover 結果、vacuity 及 pending obligations。
3. 抽離 kernel 與 demo，讓生成模型直接連結核心，不再以 include minisim.cpp 的方式測試。
4. 泛化外部刺激及可觀察交易契約，將這條 frontend 路線與 coverage-driven driver 結合；目前 differential harness 仍是 counter 專用，不宣稱任意 DUT 可自動驗收。

API 依據：[slang SourceManager](https://www.sv-lang.com/classslang_1_1_source_manager.html)、[官方 User Manual](https://www.sv-lang.com/user-manual.html)。這些是技術參考，本輪不將其列為新的 AI EDA 產業新聞。


## 常數參數增量（2026-11-03）

已支援單一flat top的elaborated default parameter/localparam：已知、非負、1..64-bit integral constants。metadata保存值、位寬、signed/local屬性，NamedValue從elaborated symbol讀取，生成常數IR；不產生假 runtime signal／coverage行。unsigned最高位為1不能當成負數，STEP=200已實測。

前端unit 7/7（含原拒絕案例及6種參數拒絕）；4-bit step1、8-bit step3、4-bit step2、8-bit step200各96筆，384筆與Icarus一致。原始測試使用相同真正Minisim kernel與existing reference harness，未改Icarus結果。證據build/parameter-frontend/summary.json。

僅default/local常數，不含instance overrides、type/string/real參數、unknown/negative/超寬參數、hierarchy或完整constant-language支援。signed runtime仍拒絕；本輪未擴充kernel。Basanos TinyALU現在越過Parameter、下一阻礙為single clock edge required（async reset多事件），仍不算native simulation PASS。


## 2026-11-03：Basanos TinyALU 真正Minisim原生執行

使用未修改的basanos/hdl/tinyalu.sv（SHA 3f41876c871b78e94a43506d99bea2cf3b1c440f457631452485406c9cd159a6），pyslang→IR→C++生成→原生Minisim執行，535筆result/done同時與Icarus及獨立Python ALU oracle一致。含32筆opcode/邊界定向刺激、500筆固定seed隨機／reset／start保持及2筆X/Z opcode default，加初始reset。乘法改加法、case比較改不等兩個生成程式mutant均被拒絕，header最後還原。證據在minisim/build/tinyalu-native，重跑minisim/tests/run_tinyalu_native.py（WSL既有venv，G++/Icarus）。

前端新增普通case使用CaseEq精確比對、first-match跳轉、分組標籤與default/no-default；只接受已知常數標籤，拒絕casez/casex/unique/priority與X/Z標籤。case frontend3/3。加入BinaryAnd/BinaryXor/Multiply映射至既有kernel運算；unsigned已知常數不再因最高位為1誤判負數。移除舊unit中已過時的多事件拒絕預期，保留其專門測試。kernel未修改。

這是TinyALU direct native harness的實際通過，不是將原500筆Icarus/pyuvm結果冒充Minisim，也不是通用cocotb/VPI或Basanos pyuvm suite已接上Minisim；該整合仍待泛化DUT handles／manifest。完整coverage closure、formal無界proof、任意case語意及其他IP仍未完成。下一步補分組/default/hold動態差異與擴充可重用DUT bridge；既有portfolio優先／OpenTitan及OpenCores次序保留。不push/merge。
