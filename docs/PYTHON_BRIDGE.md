# Python 外部驅動介面：cocotb 整合第一階段

目前最新：除了 ctypes bridge，**已在實驗性 flat-counter VPI host 執行真正 cocotb 2.1.0 與 pyuvm 5.0.0**，包含正向 scoreboard 與 mutant 拒絕。它仍不是任意 RTL 的通用 cocotb backend。以下保留各階段契約；第三階段詳見文末。

## 已實作與契約

- `Sim::advance_to(end)`：先排空目前時間的 active／inactive／NBA，再執行到 end 之內的 timer，最後將閒置 kernel 時間推到 end；拒絕倒退。若模擬已 finished，不繼續推進。
- 同一時間由外部 `set` 喚醒的程序，即使 time wheel 沒有新 timer，也會在 advance 時執行。讀取在 advance 返回後可看到此子集的穩定值；這不是 cocotb ReadOnly callback 實作。
- VCD header 只初始化一次；連續 advance 不重新開啟／截斷波形。resume budget 跨同一時間的重入累計，時間改變才重設。
- `tests/python_counter_bridge.cpp` 提供 ms_init／ms_write／ms_read／ms_advance／ms_time／ms_error。C ABI 捕捉例外轉為 -1 與錯誤字串；訊號四態使用 v/x/z masks，z 是 x 的子集。
- 此 bridge 只接受 counter 的 clk／rst 寫入，cnt 為唯讀；信號名稱由實際 generated design 查找，未知名稱回報錯誤。這是明確的 fixture，尚無通用 port direction、hierarchy enumeration 或多實例管理。
- 每個 Python worker 載入一個 shared library、只初始化一次；未支援同一 Sim reset/reload、thread safety、DLL 跨平台分發。此次在 WSL Linux／G++ shared library 執行。

## 驗收與重跑

```sh
python tests/run_python_bridge.py
python tests/run_core_tests.py
```

沿用隔離環境的 pyslang 12.1.0，需 G++ C++20、Icarus 12.0。clean 4-bit、width8、increment2 三個原始 RTL 版本各 96 筆，合計 **288 筆和 Icarus 完全一致**；兩個變體的結果確實不同於 clean，避免固定 demo 值假通過。

另外檢查：初始化 cnt 為未知、X/Z 輸入讀回、clk 變化後未 settle 前 cnt 不變、同時刻 advance 後 NBA 更新、閒置時間前進、時間倒退拒絕、未知訊號拒絕、輸出禁止寫入、重複初始化拒絕、VCD 僅一份 header。核心變更後既有 **19/19** 通過。

產物在 `build/python-bridge/summary.json`，逐案例保存 source SHA、design.json／hpp、shared library、native/reference traces 與 dump.vcd。這是 counter 專用的端到端證據，不宣稱可接任意 RTL。

## 第二階段：原生 callback 已驗收

已加入一次性 timer、value-change／POS／NEG、ReadWrite、ReadOnly callback；回傳不重用的 ID，可取消未觸發或已排隊但尚未 dispatch 的 callback。timer 必須為正延遲且不可溢位。callback 被移除後才呼叫，重複註冊須明確 rearm。取消的 timer 目前可留下沒有作用的時間點，尚未提供 NextTimeStep API。

順序契約：timer 到期及值變化 callback 優先於被喚醒的 HDL process；active／inactive／NBA 排空後才進 ReadWrite，若其產生新 HDL／NBA 則重新排空，最後執行 ReadOnly。ReadOnly callback 及其結束後同一時間都不可 set／drive／NBA／spawn，往後推進時間才解除；目前有這項保護的方法不是整個 Sim 物件所有 public 容器的安全封裝。ReadOnly 內不可註冊同時刻 RW/RO，但可註冊未來 timer／edge。

同一時刻 callback 上限 `max_callbacks_per_slot` 預設 100,000，為正整數；防止 callback 持續自我註冊卡住。禁止從 callback 重入 advance/run。callback 中的 C++ 例外會停止模擬並向呼叫者傳遞；ctypes callback 的 Python 例外不能依賴 C++ 捕捉，測試 trampoline 明確收集並使 worker 失敗。

C ABI 新增 `ms_callback(kind,signal,delay,edge,fn,token,id)`，kind 0/1/2/3 分別 timer/edge/RW/RO，edge 0/1/2 為 ANY/POS/NEG；`ms_cancel(id)` 回報是否成功取消。Python 呼叫者必須保留 CFUNCTYPE 物件，直到 callback 觸發或取消；此 fixture 沒有自動管理 Python 物件生命週期。

原生 test_callbacks 驗證排序、RW 產生 NBA 後 RO 值、取消、once、禁止寫／重入與自我註冊上限。核心 runner **20/20** 通過。真實 generated counter 的 Python callback worker 驗證 4 週期、16 個有序 callback 與 samples=[0,1,2,3]，加上三 RTL 版本 **288 筆仍與 Icarus 一致**。證據 `build/python-bridge/clean/callbacks.json` 與 summary.json。這些是 Minisim callbacks，**沒有冒充 cocotb trigger，也尚未執行 cocotb／pyuvm**。

本階段並非完整 IEEE event-region 模型；特別是同一段 native C++ 在不 yield 下連續多次 set，再返回排程器的情況，尚未驗證與 cocotb 所有 value-change 觀察語意一致。

## 第三階段：真正 cocotb／pyuvm counter VPI host

`tests/vpi_counter_main.cpp` 實作本 counter 所需的 16 個 VPI imports，以標準 vpi_user.h 編譯；透過 dlopen、vlog_startup_routines 及 cocotb 2.1.0 實際 GPI_USERS 初始化 Python／cocotb。沒有取代 cocotb triggers 或修改 cocotb 套件。固定 top 名稱 counter，訊號來自真正 pyslang-lowered generated design；root/signal/value/time/callback 路線已有動態證據。

實測：`python tests/run_cocotb_minisim.py` 的 clean 96 cycles 通過（含 edge 前舊值、ReadOnly 後新值），+2 mutant 在 sample index 1、15 ns 被 `counter mismatch` 拒絕。每個案例要求獨立 XML、恰一個 testcase、無 skip/error；mutant 必須有指定 scoreboard failure，host/bootstrap failure 不算成功抓錯。證據 build/cocotb-minisim/summary.json、逐案例 results.xml／simulation.log／trace.json。

`python tests/run_pyuvm_minisim.py` 重用上述已編譯的 clean／mutant binaries，直接執行原有 `tests/pyuvm/counter_test.py`，實際走 pyuvm build/run/check phases；clean **96 samples、3 wraps、7 resets** 與全部 16 count values 通過，mutant 在 sample=2 拒絕。證據 build/pyuvm-minisim/summary.json、XML／log；同一 scoreboard 仍可用於 Icarus／Verilator。此 fixture 沒有單獨宣稱完整 driver／monitor／sequencer 生態驗收。

限制：只驗證 flat counter／unsigned 1..64-bit 子集，1 ns precision；未支援 indexed handles、arbitrary hierarchy、arrays、force/release、非零 delayed VPI writes、DPI 或混合語言；標成 unsupported 的操作終止 host 並非默默成功。可選 root instance iteration 在現版 cocotb 啟動有 warning，top by-name 路線通過；仍需通用化 discovery。callback handle pool 保留至程序結束，尚未壓力測試長時間記憶體用量。沒有完整 VPI／IEEE event-region 相容性證明，不能拿這個 fixture 直接跑任意 portfolio IP。

下一步：抽離 counter-specific ports/top，定義可重用 DUT manifest、支援 discovery 與 handle 生命週期，擴展多訊號／相依組合邏輯 race 測試，再加入真正 pyuvm driver/monitor 交易分層。

介面盤點（台北 2026-11-03）：`python tools/probe_cocotb_interface.py` 檢查實際 cocotb 2.1.0／pyuvm 5.0.0；已安裝 wheel 有 gpi.h、沒有 include/gpi_priv 定義。已從 libcocotbvpi_icarus.so 取得 16 個必要 VPI imports，保存 library／header SHA；tests/vpi_header_probe.cpp 的標準 VPI time/vector/callback 型別 syntax-only 編譯通過。證據 build/cocotb-interface.json，這不代表該 Icarus-flavor library 可直接在 Minisim 運行；adapter 必須另經 bootstrap 與 regression 驗收。

必要 imports：vpi_chk_error、vpi_control、vpi_free_object、vpi_get、vpi_get_str、vpi_get_time、vpi_get_value、vpi_get_vlog_info、vpi_handle、vpi_handle_by_index、vpi_handle_by_name、vpi_iterate、vpi_put_value、vpi_register_cb、vpi_remove_cb、vpi_scan。這份列表是已連結符號需求，不保證涵蓋所有 runtime 語意；未支援物件／操作要明確報錯。

依 [cocotb timing model](https://docs.cocotb.org/en/stable/timing_model.html) 與 [library reference](https://docs.cocotb.org/en/stable/library_reference.html)，僅有 read/write/advance 還不夠。以上 VPI fixture 以真正 scheduler 驗收下列門檻，通用化仍需更廣案例：

1. 先定義 evaluation-cycle callback 順序，驗證 edge callback 時 downstream 值尚未更新，ReadOnly 時 NBA 已完成，且 ReadOnly 不可寫。
2. 以實際 cocotb 的 Timer／RisingEdge／ReadOnly 驅動同一生成 counter，產生測試結果及相同刺激 trace。
3. 再執行 pyuvm driver／monitor／scoreboard phases，加入已知 mutant 確認拒絕；僅 import 或 runner 能啟動不算完成。

不改動 cocotb 的 API 來假造整合；不把本 fixture 與既有 Icarus／Verilator pyuvm 通過結果混算。


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


### 2026-11-09 — frontend bitwise complement verified

`tools/pyslang_lower.py` lowers runtime unsigned bitwise complement to existing native `Op::Not`. The dedicated mode in `tests/test_lognot_wide.py` passed 256 exhaustive four-state four-bit inputs, with four outputs per sample covering equal width, expansion to eight bits, truncation to two bits and five rotating 64-bit patterns. All ordered rows agree with Icarus and the independent per-bit oracle. Replacing the four bitwise complements with logical negation produced all 256 rows and disagreed on each. Seven existing frontend unit tests passed, including signed and two-state rejection. Evidence in build/frontend-bitnot-20261109, including generated RTL, traces and source SHA. Reproduce with `python tests/test_lognot_wide.py --bitwise-not --output build/frontend-bitnot-fresh` in the WSL venv. Scope is unsigned flat RTL. Older VPI build records that pin the previous lowerer remain historical evidence and require a fresh build before reuse.


### 2026-11-09 — upstream VPI port boundary

A G++ syntax-only probe against pinned upstream `56f046a` used the existing local VPI host and saved TinyALU generated header. It identified missing timer, edge, read/write and read-only callback hooks, cancellation, advance, and source coverage members. The upstream `resize` signature additionally requires sign-fill and result-signedness arguments. The compile exited 1 before simulation. Exact diagnostics and command are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-interface-audit/`.

Prioritize a narrow scheduler seam before VPI integration. Its first acceptance case should cover a timer-only slot, cancellation, one value-change callback, read/write changes that settle through NBA, and read-only after quiescence. Use the upstream event-driven single-thread path first. Then adapt the existing host to the upstream value representation and use one real cocotb counter with 96 samples plus the expected mutant rejection. Defer source coverage, compiled scheduling, and wide VPI vectors until those can be measured independently. This isolates the main behavioral risk at the scheduler boundary and keeps the validated local bridge available.


### 2026-11-09 — timer-only scheduling probe on upstream

An isolated C++ probe reused upstream `Task`, `Delay` and `Sim::spawn` without changing the scheduler. Two armed timers at tick 3 ran in registration order, a cancelled timer produced no callback, and the remaining timer fired at tick 7 after the run resumed from tick 3. The four ordered trace rows matched. The first build used the local demo macro, then succeeded with the upstream `MINISIM_NO_MAIN` guard. Sources, compiler logs and result are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-timer-probe/`. This covers timers registered before the first run. The cancelled coroutine still wakes and checks its flag. Callback rearming, region ordering and VPI are separate acceptance items.


### 2026-11-09 — same-time scheduling probe

A parent coroutine at tick 3 queued a postponed `$strobe` and registered a zero-delay child that writes `q` through NBA. With `Sim::spawn`, the child entered the time wheel and ran after the current postponed observer. The observed value was 0 while the final value at tick 3 was 1. In a separate probe, enqueuing the child in the current active queue produced observed 1 and final 1 at the same tick. Both traces match the predictions. Evidence is under `../minisim-upstream-bitnot-20261109/build/vpi-upstream-rearm-probe/`. This is a comparison of two scheduling seams on the upstream core. The direct queue access is a probe. The eventual bridge needs an explicit same-slot hook with ownership, phase and termination checks before callback rearming is accepted.


### 2026-11-09 — guarded active-slot prototype

A private copy of upstream `56f046a` admits a fresh coroutine to the current active queue only while an active coroutine resumes. Four checks passed. Its NBA settles before the postponed observer, pre-run and post-run submissions are rejected, and the single executed coroutine cleans up its local token once. The caller destroys the handle on rejection. Evidence under `../minisim-upstream-bitnot-20261109/build/vpi-upstream-active-hook/` includes the patched copy, test, hashes and logs. The prototype has a single-thread scope and assumes a fresh handle supplied once. Reentrant runs, duplicate handles, callback-region transitions and unbounded same-slot rearming remain to be resolved before a tracked runtime change or cocotb integration.


### 2026-11-09 — bounded active callback rearming probe

An isolated scheduler copy queued callables in the active slot. A callback rearmed itself until registration reached the test cap of three. At ticks 3 and 7, three callbacks executed and the fourth registration was rejected. Postponed observed the settled NBA results 3 and 6. The second tick confirmed that the cap resets per slot. The first build exposed a test helper name collision with the upstream demo. Renaming that helper allowed the probe to compile and run. Sources and logs are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-rearm-budget/`. The cap and rejection API are experimental. A VPI adapter still needs to surface rejection to the caller and define its read/write and read-only behavior.


### 2026-11-09 — read/write and read-only region probe

A private upstream scheduler copy ran one read/write and one read-only callable at tick 3. Read/write saw the first NBA value 1 and queued value 2. The scheduler settled the second NBA before postponed and read-only both observed 2. Two attempted writes from read-only were rejected through `set` and `nba`; the final value remained 2. Four ordered rows matched. Evidence is in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-sync-probe/`. This prototype has one pending callable per region. Next integration needs the bounded queues, registration and cancellation semantics, and a real cocotb run on the upstream core.


### 2026-11-09 — upstream-based VPI counter prototype

A private copy of upstream `56f046a` now runs the existing cocotb counter through the adapted local VPI host. The probe adds single-thread timer, value-change, read/write and read-only dispatch to the upstream interpreter, preserves the existing local unsigned frontend for the counter, and adapts the upstream `resize` signature. Source coverage is omitted from this experiment. The unmodified counter completed 96 scoreboard samples with a nonempty passing cocotb XML result. Source and binary hashes, generated headers, compile command and logs are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-counter-20261109/`.

This is a private single-thread integration prototype. Next require the increment-by-two RTL mutant to fail with the named counter scoreboard error. Then separate reusable callback code from harness adaptation and test budget, cancellation, reentry and write rejection on the integrated path. Pyuvm, wide values, parallel and compiled scheduling are separate scopes.


The upstream-based cocotb bridge rejected the counter RTL mutation `cnt + 1` to `cnt + 2`. The mutation was applied at exactly one source site in a separate output directory. The simulation process exited normally and produced one testcase with one named `counter mismatch` failure. The acceptance gate rejected crash, absent XML, or unrelated failure as substitutes for the expected mismatch. Clean artifact hashes matched before and after the negative run. Evidence is in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-counter-mutant-20261109/`. The prior 96-sample clean run remains the positive evidence. This validates the private single-thread upstream prototype against one real RTL mutant.


### 2026-11-09 — reentry guard on the integrated callback core

A derivative of the cocotb-validated core rejects nested `run()` through a scoped guard. At tick 1 a timer tried to run through tick 2 from inside its callback. The call was rejected before time advanced, and the tick 2 timer ran once when the outer caller resumed. An injected callback exception also released the run guard. The callback dispatcher still marks that simulation finished. These three checks ran in native C++ against a private core copy. Evidence and the original-core SHA are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-run-guard/`. The 96-sample cocotb result applies to the original prototype. Next integrate the guard and verify that reopening the same tick cannot reset its callback budget.


### 2026-11-09 — callback budget persists across segmented runs

The integrated prototype previously reset the callback counter whenever it opened a wheel entry, including a reopened tick. The guarded-core derivative now resets only when the tick changes. Three native checks passed with a cap of two. Separate runs at tick 0 retained a combined count of two. Tick 1 received a fresh budget and reached two after a timer and a separately submitted callback. The third submission at tick 1 caused the named budget error before execution. Evidence is in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-budget-persistence/`. This result applies to the private derivative. Consolidate both guards with cancellation tests before rerunning real cocotb against a candidate build.


### 2026-11-09 — guarded upstream VPI integration

Rebuilt the isolated upstream interpreter bridge with the reentry and per-tick budget guards. The existing real cocotb counter completed 96 samples with one passing XML testcase. The complete parsed transaction trace equals the previous prototype trace. Previous source and binary hashes were checked before and after execution. The fresh build, hashes and test evidence are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-counter-guarded-20261109/`. This run establishes positive integration after the scheduler changes. The earlier mutant result applies to the earlier prototype. The next gate is cancellation and negative-path validation on the guarded core before promoting it to tracked source.


### 2026-11-09 — cancellation on the guarded VPI core

A native probe used the exact core hash from the guarded 96-sample cocotb run. It cancelled a timer, an already queued edge callback, a read/write callback and a read-only callback. No cancelled function executed. A live timer executed once, consumed one budget unit and could no longer be cancelled. Repeated cancellation of the cancelled timer was rejected. Artifacts are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-guarded-cancel/`. This tests core cancellation. VPI handle lifecycle and the guarded RTL mutant are separate remaining checks.


### 2026-11-09 — guarded prototype rejects the counter mutant

The same guarded core and host used for the 96-sample positive run rejected the existing single-site increment-by-two RTL mutant. The process exited normally and the XML gate accepted exactly one testcase with the expected `counter mismatch` failure. Positive artifacts matched their saved hashes before and after the run. Evidence is in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-guarded-mutant-20261109/`. The private single-thread prototype now has positive and negative cocotb evidence and core cancellation evidence. Remaining integration work includes public VPI cancellation, a reviewable source layout, and independent checks of parallel or compiled modes before claiming those modes.


### 2026-11-09 — public VPI removal checks

The guarded host's public C VPI entry cancelled all four registered callback kinds without any of their functions firing. The control timer fired once. The same probe exposed that `vpi_remove_cb` returned success for a signal handle and marked it inactive. A private host derivative adds a kind and live check. The probe confirmed four valid removals still work, the control timer still runs, and null, repeated and wrong-kind removals return zero. The signal remains live. Evidence is in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-public-cancel/` and `../minisim-upstream-bitnot-20261109/build/vpi-upstream-cancel-kind-guard/`. The new host guard has direct C API evidence. The earlier cocotb runs pin the previous host. The next review should check completed one-shot handles and run the revised host through cocotb before promotion.


The host with the callback-kind and duplicate-removal guard completed all 96 samples in the existing real cocotb counter. Its full parsed trace matches the guarded baseline, and one XML testcase passed. Evidence is in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-cancel-guard-cocotb/`. The separate C API probe covers invalid removals. Completed one-shot handle semantics are still open. The earlier RTL mutant evidence pins the host before the kind guard.


### 2026-11-09 — one-shot retirement and self-cancellation

The private VPI host clears the dispatched native callback ID and retires one-shot handles after invocation. A value-change callback can still remove itself during dispatch. Three C API checks passed: removing an already completed timer returns zero, removing the edge from inside its callback succeeds, and the next signal transition does not fire or rearm that edge. The scheduler callback registry is empty at the end of the probe. Evidence and source hashes are in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-oneshot-lifecycle/`. The 96-sample cocotb result pins the earlier host. Next validate this host in cocotb before consolidating the prototype into reviewable tracked changes.


The one-shot retirement host completed 96 real cocotb counter samples with one passing XML testcase. The complete parsed trace equals the guarded baseline. The host and core hashes match the direct C API lifecycle probe. Evidence is in `../minisim-upstream-bitnot-20261109/build/vpi-upstream-oneshot-cocotb/`. The next candidate should consolidate the tested scheduler and host changes in tracked source with a reproducible build entry. Keep the test-only complement commit separate.


### 2026-11-09 — reviewable scheduler candidate

The isolated `codex/upstream-vpi-scheduler-20261109` branch at `56f046a` has a working diff in `src/minisim.cpp`, `CMakeLists.txt` and `tests/test_callback_scheduler.cpp`. It brings together the tested timer, edge, read/write, read-only, cancellation, reentry and per-tick budget changes. The candidate stops the slot on finish requested from a callback and retains the existing normal RTL finish loop. The CMake Debug build and both focused CTest targets passed. `test_callback_scheduler` checks cancellation, NBA settling, read-only write rejection, reentry and budget persistence. Existing `test_sched` exercises the native scheduler. Evidence is under `../minisim-upstream-vpi-20261109/build/callback-scheduler/`. The first CTest invocation failed at shell argument parsing. Both tests ran successfully with separate filters. The candidate is local, with the host and real cocotb verification against this exact diff next. Prior cocotb results apply to their separately hashed prototypes.

### 2026-11-09 — cocotb on the scheduler candidate

The working tree on `codex/upstream-vpi-scheduler-20261109` (base
`56f046a`) passed the real cocotb counter regression. The private
one-shot lifecycle host used the candidate's `src/minisim.cpp` directly.
The candidate source hash matched before and after the run. JUnit reported
PASS and all 96 samples matched the previous lifecycle run exactly.
The build and simulation evidence is in
`minisim-upstream-vpi-20261109/build/callback-scheduler-cocotb/` under the
shared workspace root. This run covers the flat counter with the private
VPI host and a generated header. The next acceptance check is the
`cnt + 2` mutant on this same candidate and host, with a completed run
and the expected scoreboard mismatch in JUnit.

### 2026-11-10 — scheduler candidate rejects the counter mutant

The same scheduler source and private host as the 96-sample positive run
rejected the single increment-by-two mutation. The generated design
differed only in the increment literal. The simulator exited normally.
The positive gate rejected the XML, and the expected-failure gate accepted
its sole testcase at sample index 1 (15 ns), expected 1 and observed 2.
The first launch lacked the trace path and produced a setup failure.
Its XML and log are retained separately from the verified result. The
earlier preflight stopped before compilation because it checked the wrong
literal spelling. The verified XML, two samples, command and hashes are
under `minisim-upstream-vpi-20261109/build/callback-scheduler-cocotb-mutant/`
in the shared workspace.

This gives the scheduler diff positive and negative flat counter evidence.
Defer a PR number until the private host has a tracked, reproducible build
entry and the public callback lifetime checks run through that entry.

### 2026-11-10 — opt-in CMake counter host

On `codex/upstream-vpi-scheduler-20261109`, the previously verified host
is now in `tests/vpi_counter_host.cpp`. CMake builds it only when
`MINISIM_VPI_DESIGN_DIR` points to a directory with a generated
`design.hpp`. The host and lifecycle probe require Linux and the Icarus
VPI header. The simulator loads the installed cocotb VPI library at runtime.

From the workspace root in the existing WSL Python environment, generate
the flat counter header using the adjacent Minisim lowerer. This adapter
omits local IR line coverage fields and maps the unsigned resize helper
to the candidate API.

```python
from pathlib import Path
import sys

candidate = Path("minisim-upstream-vpi-20261109").resolve()
sys.path.insert(0, str(Path("minisim/tools").resolve()))
from pyslang_lower import Lowerer, cpp_header

model = Lowerer(candidate / "examples/counter.sv").lower()
for instructions in model["programs"]:
    for instruction in instructions:
        instruction.pop("line", None)
design = candidate / "build/vpi-counter-fresh/design"
design.mkdir(parents=True, exist_ok=True)
(design / "design.hpp").write_text(
    cpp_header(model).replace("resize(", "bridge_resize("), encoding="utf-8"
)
```

From the candidate checkout, build and test with the same directory.

```sh
cmake -S . -B build/vpi-counter-fresh -DCMAKE_BUILD_TYPE=Debug \
  -DMINISIM_VPI_DESIGN_DIR="$PWD/build/vpi-counter-fresh/design"
cmake --build build/vpi-counter-fresh --parallel 1 \
  --target minisim_vpi_counter test_vpi_callback_lifecycle
ctest --test-dir build/vpi-counter-fresh -R test_vpi_callback_lifecycle --output-on-failure
```

Both targets built. CTest passed one lifecycle testcase covering completed
timer removal, removal from inside a value-change callback and no
rearming on the next edge. The CMake-built executable passed one real
cocotb XML testcase and all 96 trace rows match the prior counter run.
Regenerating the header from the checkout's RTL produced identical bytes.
The evidence is in `minisim-upstream-vpi-20261109/build/vpi-counter-cmake/`.
The initial evidence script indexed the process list incorrectly and
stopped before simulation. The corrected script completed both checks.

This entry still depends on a flat header from the adjacent lowerer.
The increment-by-two rejection remains pinned to the previous direct
build. Next, exercise the mutant through CMake with the same generation
recipe and consolidate the counter entry with the shared runtime and XML
helpers before PR review.

### 2026-11-10 — CMake counter mutant and PR scope

The opt-in CMake target built from a private copy of the counter RTL with
one change from `cnt + 1` to `cnt + 2`. Lowering regenerated the header
with only the increment literal changed relative to the clean CMake
build. The simulator exited normally. Cocotb reported one XML failure
at 15 ns, sample index 1, expected 1 and observed 2. The ordinary success
gate rejected the XML and the expected-failure gate accepted the exact
scoreboard diagnostic. Both sampled rows and source/binary hashes were
saved under `minisim-upstream-vpi-20261109/build/vpi-counter-cmake-mutant/`.
The existing clean CMake artifacts were checked by hash and left in place.

GitHub lists [PR #16](https://github.com/tonythetiger168/minisim/pull/16)
as an open documentation-only draft. The scheduler and opt-in flat VPI
host are suitable for a separate implementation PR once the existing
counter entry can reproduce both CMake runs and the default native suite
has passed on the proposed revision. Link that PR to #16. This evidence
covers the single-thread flat counter and direct callback lifecycle probe.
The adjacent pyslang lowerer is still required. No background job was
started for this test.

### 2026-11-10 — default native regression for the scheduler candidate

A fresh single-worker CMake build on
`codex/upstream-vpi-scheduler-20261109` passed all 7 default CTest
cases. The suite exercised expression values, Z logic, the scheduler,
nets, system tasks, callback scheduling and the demo VCD. The CMake
cache confirmed that both the optional slang frontend and VPI build
were disabled. Source hashes matched before and after the run.
Commands, logs, JUnit and hashes are under
`minisim-upstream-vpi-20261109/build/native-default-20261110/`.
The optional cocotb and lifecycle evidence remains in the separate CMake
runs above. No portfolio job was started. The remaining PR preparation
step is to make the existing counter runner reproduce the CMake clean
and increment-by-two results in one command.

### 2026-11-10 — one-command CMake counter regression

From the shared workspace root with the existing WSL environment
activated, run the existing counter runner with a new output directory.

```sh
python minisim/tests/run_cocotb_minisim.py \
  --cmake-candidate minisim-upstream-vpi-20261109 \
  --output minisim-upstream-vpi-20261109/build/vpi-counter-fresh-run
```

The option lowers and builds both cases through CMake and uses the shared
runtime, XML and provenance helpers. It requires a fresh output directory.
The positive case passed one XML testcase and 96 samples. The trace SHA
matches the earlier CMake run. The mutant exited normally and its one XML
testcase reported the expected mismatch at sample index 1, expected 1 and
observed 2. The generated headers differ only in the increment literal.
Both cases saved separate commands, logs, XML, trace and hashes under
`minisim-upstream-vpi-20261109/build/vpi-counter-one-command/`.
The original build profile also passed 96 samples and caught the mutant
at the same point, using a fresh `minisim/build/vpi-counter-runner-legacy-20261110/`.

Together with the 7 default native tests and the direct lifecycle probe,
this meets the local acceptance criteria for a separate implementation
PR linked to tracking PR #16. The runner and lowerer still live in the
adjacent experimental checkout. A self-contained clean-checkout review
needs that dependency explicitly packaged or documented. Scope remains
the single-thread flat counter. No portfolio job was started.

### 2026-11-10 — callback limit reduced within one tick

The candidate now rejects callback dispatch when the consumed count is
at or above the current limit. The previous equality check let a
third callback run after lowering the limit from two to one at the
same tick. The focused test in `tests/test_callback_scheduler.cpp`
reproduced the failure before the fix and passed after it. The
scheduler reports the budget error before running the third callback.
Evidence is in
`minisim-upstream-vpi-20261109/build/callback-budget-lowered-20261110/`.
One targeted native test ran before and after the change. No cocotb or
portfolio run was started. The prior 7-test default result and CMake
cocotb results pin the earlier scheduler source. Refresh those checks
against the revised source before marking the implementation ready for
PR review.

### 2026-11-10 — revised scheduler acceptance

The callback-budget fix passed all seven default native CTest cases
and the separate VPI callback lifecycle test. The one-command CMake
runner passed the 96-sample clean counter and rejected the increment-by-two
mutant at sample index 1 with the expected scoreboard mismatch. Both
trace hashes match the prior run. The input snapshot and both VPI
provenance records verified on the revised source. The result collector
initially rejected the formatting of the CTest output. A corrected
collector validated the saved artifacts without another test run.
Evidence is in `minisim-upstream-vpi-20261109/build/budget-fix-acceptance-20261110/`.
No portfolio job was started. The next step is a local review of the
implementation and the documented companion checkout dependency.

### 2026-11-10 — isolated companion CLI preflight

The six source files pinned in the scheduler candidate's
`docs/VPI_COUNTER.md` were copied to a temporary directory. With the
existing Python environment and isolated Python mode, the runner
showed help, rejected an implicit output path for CMake, preserved an
existing output directory on refusal, and rejected candidate
mutant-only mode. Four CLI checks passed. This validates the companion
source list and output guards through the entry point. It did not
compile a simulator or rerun cocotb. Evidence is in
`minisim-upstream-vpi-20261109/build/companion-entry-preflight-20261110.json`.
I2S remains prepared and unattempted at 319724 KiB Windows free memory.

### 2026-11-10 — local scheduler review patch

The seven candidate files were packaged as a local patch using a
temporary Git index. The patch applied to `56f046a` in a separate
index and reproduced the same diff byte for byte. The real worktree
status was unchanged, and the saved native and lifecycle input
hashes verified. Patch and review metadata are under
`minisim-upstream-vpi-20261109/build/scheduler-review-20261110/`.
This packages the scheduler, opt-in VPI host, native tests and
documentation. The six companion source files remain a separate
dependency for cocotb. No tests reran and no PR was created.

I2S remains prepared. WSL had 6493 MiB available, but Windows had
237636 KiB free at the launch check. No portfolio container started.

### 2026-11-10 — companion sources packaged for local review

The review directory now includes `cocotb-companion-sources.zip`
beside `scheduler-vpi.patch`. The archive contains exactly the six
pinned source paths required by the companion checkout. Its CRC and
all six byte hashes verified against the documented pins and the
prior four-case CLI preflight. See
`minisim-upstream-vpi-20261109/build/scheduler-review-20261110/companion-package.json`.
The existing Python packages, CMake, compiler and VPI header remain
environment dependencies. No build or simulation reran. I2S did not
start at 212624 KiB of Windows free memory.

### 2026-11-10 Shared VPI build output lock

The Linux/WSL shared builder holds a nonblocking advisory lock from before
lowering through provenance verification. A competing build into the same
resolved output directory fails before it writes generated files or invokes
the compiler. The lock file stays in place and the OS releases the lock when
the file descriptor closes. Use distinct output directories for parallel
jobs and for runs that overlap a rebuild.

The new regression reached lowering before the fix. After the fix, nine
build tests passed with a mock compiler, including two successive successful
builds that each reject a competing call. A separate two-process WSL probe
confirmed contention and release on the Windows-mounted build directory.
No RTL simulation was run. This lock coordinates shared-builder calls only.
Existing provenance that pins the previous builder remains historical
evidence and requires a fresh build for reuse.
