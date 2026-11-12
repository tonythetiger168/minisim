# Minisim Coverage-driven 驗證

2026-11-01 實作與驗收。分開記錄各類 coverage 的分母、命中及未覆蓋項；coverage 命中不是功能正確證明。

## 支援矩陣

| 類型 | Minisim 原生實作 | Verilator 5.032 參考驗證 |
|---|---|---|
| Line | `Instr.line_bin` 由 exec 收集；未執行的已登錄 bin 留為 0。新增 pyslang 有限子集前端可從真實 AST 自動產生來源行映射，counter 兩行已端到端驗證；尚非任意 RTL 支援 | `--coverage` 收集真實 RTL line/branch；保留 raw dat 與 LCOV |
| FSM | `FsmCoverage` 預先宣告合法 states/arcs，取樣非法狀態／轉移報錯；reset_history 可切斷 reset 邊界 | 真實 3-state FSM，以顯式 cover property 記錄 3 states／3 arcs；未宣稱 5.032 有自動 FSM extraction |
| Toggle | 模擬核心 `Sim::set` 逐 bit 分別記錄 0→1、1→0；X/Z↔已知值不當作 known toggle；開啟後再註冊訊號 | 原生 RTL toggle counters。與 Minisim 雙向 bins 的分母不同，不直接比較百分比 |
| Assertion | C++ `ClockedProperty` 取樣 API，提供同一取樣與下一取樣的 implication monitor；記錄 attempts/pass/fail/vacuous/disabled/pending | `--assert` 真正編譯並執行 RTL `assert property`；錯誤 FSM 轉移在 ap_run_done 被拒絕 |
| Functional SVA | 原生 cover monitor 只將真正成功的 obligation 算 cover hit；無 antecedent 命中不算 closure | `cover property` 狀態及前後狀態 witness。arc 使用 `$past` 與目前狀態的 conjunction，避免 implication 的 vacuous hit |
| Functional bins | `CoverageGroup` 明確定義 bins、累計次數、查 missing；driver 依 live holes 選擇 reset／increment，循環至閉合或預算上限 | pyuvm 96-cycle scoreboard 與 RTL 參考 coverage 分開保留 |

Minisim 的 property API 不是 SVA 語法 parser；呼叫者指定 sampling event，範例在 NBA 結束後取樣，不聲稱具備完整 SVA preponed/observed regions。Unknown consequent 算失敗；unknown antecedent 亦採保守失敗策略。支援單一取樣或下一取樣的 C++ monitor，不包含任意 `##N`、repetition、throughout、until、sequence composition、完整 covergroup/cross 或完整 IEEE 1800 符合性。

## 實際結果

Native coverage-driven counter 在 **20 cycles** 內完成：

- 2/2 明確映射 line bins。
- 12/12 directional toggle bins（clk/rst/cnt 共 6 bits，每 bit 兩方向）。
- 2/2 抽象狀態 ZERO／ACTIVE、4/4 指定轉移。這是 counter 的觀測模型，不是 RTL FSM 自動抽取。
- 20/20 functional bins（16 個計數值、increment、wrap、zero/nonzero reset）。
- count_correct 20 次真正成功、0 失敗；wrap property 1 次真正成功、0 失敗、16 vacuous、3 disabled、0 pending。Vacuous 不計 cover hit。

原生測試還驗證空分母不能閉合、未知 bin 不可默默加入、非法 FSM arc 被拒絕、cover 的 vacuity、unknown 失敗、disable 中止 pending，以及下一取樣失敗。

Verilator reference 跑 **80 cycles**：

- 6/6 顯式 FSM state/arc cover properties 命中。
- IDLE→RUN、RUN→DONE、DONE→IDLE 各 5 次實際 witness。
- no-start 負向案例：IDLE 命中，RUN/DONE 及三條轉移皆 0，不能誤報全覆蓋。
- BUG=1 將 RUN→DONE 改成 RUN→IDLE，`ap_run_done` assertion 實際失敗，runner 必須辨認指定 assertion，而非把任意 crash 算成功。
- 收到 v_line、v_branch、v_toggle、v_user 原始 counters。包含未覆蓋的非法/default 分支，不宣稱 RTL 所有項目 100%。

建置：WSL Ubuntu、G++ 15.2.0、Verilator 5.032；CMake 4.4.4 Release 實際建置與 CTest **12/12 通過**。Python/pyuvm 環境見 PYTHON_ENVIRONMENT.md。

## 重跑與產物

在 WSL repo 根目錄：

```sh
python3 tests/run_core_tests.py
python3 tests/run_native_coverage.py
python3 tests/run_verilator_coverage.py
# 在已安裝 pyuvm/cocotb 的隔離環境：
~/.venvs/ai-eda/bin/python tests/run_pyuvm_counter.py --sim verilator --coverage-dir build/coverage/verilator
```

`build/coverage/native/coverage.json` 為 Minisim 各群 bins、計數與 property 結果。
`build/coverage/sva/` 保留 summary.json、clean.dat、clean.info、no-start.dat、建置與執行 log。clean 和 mutant 不混合累積。
`build/coverage/verilator/` 為 pyuvm 驅動 counter 的 Verilator raw/LCOV；與上面 FSM fixture 不同。

## 下一階段

14:05 跟進已將 pyslang-lowered counter 與 adaptive coverage driver 接合，且相同刺激在 Icarus／Verilator 一致。正向、錯誤 RTL 與預算耗盡三種結果均獨立驗證，詳見 [ADAPTIVE_FRONTEND.md](ADAPTIVE_FRONTEND.md)。原本「兩條路線尚未合併」的限制只描述較早的 13:01 狀態；目前合併範圍是 2..8-bit counter 驗證模型。

同日 13:01 排程已完成有限子集的 pyslang→來源映射 IR→Minisim 執行；288 筆與 Icarus 一致。產物及支援邊界見 [PYSLANG_FRONTEND.md](PYSLANG_FRONTEND.md)。既有 native coverage-driven counter 仍是手工 IR driver；新 frontend differential harness 尚未與自適應 stimulus 合併，避免混稱完成。

1. 將完整 coverage model 從測試 fixture 抽成可供不同 DUT 使用的監測介面，強制 reset 與 clock 的取樣契約。
2. 接通 pyslang→可執行 IR 與來源映射，再讓任意支援子集 RTL 取得 line coverage；不將缺失映射算已覆蓋。
3. 為 SVA 明確列出可接受的 syntax subset；先處理 `assert/cover property`、指定 clock、disable、單步 implication，遇到其餘語法明確報 unsupported。
4. 擴充 functional cross、FSM reset arc policy、可合併 run manifest 與持續未覆蓋目標；以 SHA、模型與刺激版本隔離分母。
5. 比較相同刺激下的 Minisim／Icarus／Verilator 交易及 coverage；四態 X/Z 語意仍以四態參考案例驗證，不用 Verilator 的 known-value counter 結果取代。

技術來源：[Verilator Coverage Analysis](https://verilator.org/guide/latest/simulating.html#coverage-analysis)。官方 latest 文件可能描述較新功能；本機能力以 5.032 實跑為準。


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


## 2026-11-06 12:43 Taipei：RTL property結束條件評測缺口

低資源工作僅重新解析既有SVA raw evidence：clean六個cover hit與summary一致、no-start六項一致，mutant log含指定ap_run_done；證據build/sva-evidence-audit-20261106.json保存四份原檔SHA。這是歷史資料稽核，新增模擬0，不增加原有測試數。

具體缺口：tests/rtl/coverage_harness.cpp目前80 cycles後直接final；不能由此宣稱RTL property pending obligation為0。src/coverage.hpp C++ ClockedProperty.has_pending驗收屬另一介面，不外推真正RTL SVA。P1下一評測須在固定刺激後停止新antecedent、至少完成已知一拍obligation的drain，並建立末拍觸發、drain前失敗／drain後錯誤consequent被拒絕的正負例；若加顯式shadow monitor須標示其獨立模型身分，不稱Verilator內部pending讀值。驗收保存sample時序、trigger/resolution計數、RTL assertion結果，未實作／未PASS前不提高closure聲明。

host可用385472KiB約376MiB，Portfolio exited、其他buildkit未動，CSE未attempt；仍10已量測、4partial、116無紀錄、full0。優先P0資源可行時CSE單項，P1再處理上述Minisim/參考property評測，避免重跑既有PASS。未推送。


### 2026-11-07 09:45 Taipei — PR #16 temporal-obligation fixture design

Inspected the actual RTL implication and 80-cycle harness. Independent two-state schedule checks cover five cases: last-cycle pending, one-edge drain, wrong consequent, no antecedent, and reset cancellation. All five expected model outcomes were checked; this is schedule-design validation only, not new RTL simulation or formal proof. Starting at cycle 2 yields RUN/cnt=3 at sampled cycle 6; ending there leaves an obligation, cycle 7 resolves it. Reset cancellation must not manufacture a nonvacuous resolution. Source SHA and complete sampled timelines are saved in build/rtl-obligation-plan-20261107/schedule-cases.json. Next implementation should extend the existing harness (no duplicate runner), sample before rising-edge eval, and execute actual assertion clean/mutant tests. Draft tracker: https://github.com/tonythetiger168/minisim/pull/16 .


### 2026-11-07 10:15 Taipei — actual RTL end-obligation regression

Extended existing coverage_harness.cpp and run_verilator_coverage.py, without a duplicate runner. Eight end-case executions (clean/mutant x drained/undrained/no-start/reset) matched expected outcomes. Clean drained resolves one trigger; undrained returns 2 with pending=1; no-start and reset-cancelled return 3 for missing nonvacuous resolution. Drained BUG mutant is rejected by the actual RTL ap_run_done assertion. Shadow pending is an independent fixture monitor, not Verilator internal state. Existing six state/arc cover checks, no-start zero-hit check, and ordinary assertion-mutant rejection also passed. Raw logs, coverage and source/log SHA are in build/coverage/sva-end-obligation-20261107/end-obligations.json. This is Verilator reference RTL validation, not a native Minisim SVA parser or full closure. PR #16 follow-up implementation remains to publish/review.


### 2026-11-07 10:45 Taipei — strict obligation-result acceptance

Existing runner now validates exact trigger/resolution/cancellation/pending/failure counts and the complete ordered sample sequence, rather than only matching an exit code and OBLIGATION marker. All eight saved real RTL execution logs accepted; six corrupt evidence variants rejected (empty, missing resolution, pending obligation, duplicate summary, duplicate sample cycle, wrong exit). No RTL simulation repeated. Evidence: build/coverage/sva-end-obligation-20261107/validator-audit.json. Actual mutant signal code is not hardcoded for acceptance: nonzero plus the intended ap_run_done assertion and complete pre-abort samples are required. Git diff check passed. PR publication remains pending the previously requested authorization; no external write retried.


### 2026-11-08 — functional supplement shared lifecycle accepted

Migrated existing tests/run_alu_holes_vpi.py to shared build/runtime/provenance/XML helpers. It builds the unchanged Basanos TinyALU with the directed test, fixed-bin analyzer, baseline TX and plan included in provenance, verified before/after simulation. Fresh --output is mandatory in practice (existing directory rejected); --baseline/--plan enable explicit inputs. Actual cocotb execution produced31 valid directed samples: historical500 samples21/55 plus new31 gives531 samples55/55. This reproduces known functional closure through the maintained shared lifecycle, not new bin coverage or a single global timeline. Stale plan, altered stimuli and existing-output negatives all rejected before build; prior output preserved. Evidence: build/alu-holes-shared-20261108 (summary, XML, logs, provenance, guard-results.json). Historical artifacts untouched. Next: migrate toggle supplement while preserving exact baseline source/binary/bin identity; do not rebuild it silently.


### 2026-11-08 — toggle supplement identity migration accepted

Migrated existing run_toggle_holes_vpi.py to shared runtime/provenance/XML acceptance, explicit --baseline/--output and fresh output directories. Historical baseline was correctly rejected as stale (vpi_generic_main.cpp changed); it was not rebuilt or relabelled. Added --output and a binary/toggle/provenance hash record to the existing coverage runner, then created a separate128-sample baseline: line12/12, toggle76/78. Two real cocotb directed samples on that same verified binary close the exact78-bin union to78/78. Baseline and supplement verified before/after execution; supplement test hashes retained. Three pre-simulation negatives passed: historical stale baseline, modified toggle file, existing output (prior evidence preserved). Evidence: build/alu-toggle-baseline-shared-20261108 and build/alu-toggle-supplement-shared-20261108 (summary/XML/provenance/guard-results.json). Revalidation of maintained entry integration, not new semantic coverage or a single global timeline. Content hashes are local consistency evidence, not signed/hermetic provenance or proof that arbitrary fabricated reports are genuine.

### 2026-11-11 — Consecutive triggers in the C++ property API

A targeted regression in tests/test_properties.cpp passed under WSL G++.
The second trigger resolves the first obligation and leaves a newer one
pending. Cover has a hit at that point and assertion acceptance stays
false. A false consequent on the next sample clears the pending state
and records one failure. This exercises the native C++ monitor. RTL
assertion parsing and execution are separate test paths.

The I2S directed run remains prepared. Windows reported 1621224 KiB
available and the Docker Desktop Linux engine endpoint was absent at
preflight. The next I2S acceptance is 18 checks and three target point
gains against the saved baseline.
