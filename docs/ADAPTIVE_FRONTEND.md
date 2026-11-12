# 真實 RTL 前端與 Coverage-driven 刺激整合

2026-11-01 14:05（Asia/Taipei 排程觸發）跟進，延續 codex/minisim-four-state-mux 本地分支。開始時保留所有先前修改，未 reset、push、merge 或發布。

## 本輪改進

前一輪已能執行 pyslang 產生的 IR，但 coverage-driven driver 仍只連接手工 IR。本輪新增實際路線：

`RTL → pyslang AST → generated IR/C++ → Minisim → live coverage holes → 下一週期刺激`

驗證模型與 DUT 分開：`tests/support/counter_model.hpp` 是 +1 同步 reset counter 的 golden model、functional bins、抽象 FSM 與 clocked-property monitors；DUT 只來自真正降低後的 RTL。模型以尚未覆蓋的 value／wrap／reset bins 選擇下一次 reset。這是可重用的 **2..8-bit counter 驗證模型**，不是任意 DUT 的通用求解器。

錯誤樣本先由 golden scoreboard 拒絕，不能增加 functional coverage。line/toggle 仍如實記錄實際執行與切換；即使結構 coverage 很高，golden 失敗也不能整體通過。預算耗盡、缺失 mapped lines、空分母、未完成 property、未命中的 functional bins 都不能當作閉合。

## 實際驗收

| 案例 | Minisim 取樣數 | Golden／closure 結果 | 同一組刺激的參考比對 |
|---|---:|---|---|
| clean4 | 20 | 通過，20 functional bins、2 line、12 directional toggle、2 state／4 arc 閉合 | Icarus、Verilator 逐筆一致 |
| clean8 | 260 | 通過，260 functional bins、2 line、20 directional toggle、2 state／4 arc 閉合 | Icarus、Verilator 逐筆一致 |
| increment=2 mutant | 3 | 指定 scoreboard_mismatch、非零退出；錯誤值未計入 functional coverage | 仍與該錯誤 RTL 在 Icarus、Verilator 的結果一致 |
| hold-reset stalled | 8 | cycle_budget_exhausted、非零退出；coverage 未閉合 | Icarus、Verilator 逐筆一致 |

共 **291 筆**執行結果同時匹配兩個參考模擬器。mutant 的「參考一致」只證明三個引擎執行相同錯誤設計，不代表其符合 golden 規格；這兩個條件刻意分開驗收。

所有 case 的刺激均由 Minisim 的 live coverage 狀態產生，之後才 replay 到 Icarus／Verilator，沒有另生成一組「看似相同」的隨機刺激。native trace、兩個 reference trace、coverage、IR、RTL 與錯誤訊息均保留。manifest 記錄 RTL／stimulus／generated-header／實際 kernel 與 verifier 檔案 SHA，以及 Python/pyslang/G++/Icarus/Verilator 版本；因本地尚有未提交變更，不能只靠 git HEAD 重現。

## 重跑

在 WSL repo 根目錄：

```sh
~/.venvs/ai-eda/bin/python tests/run_coverage_driven_frontend.py
```

需要既有 pyslang 環境、G++、Icarus、Verilator。輸出位於 `build/coverage/adaptive/`，各案例獨立目錄；`summary.json` 是整體結果與 provenance。Verilator 原始 `.dat` 與建置 log 同樣保留。模擬與編譯有時間／週期限制，缺少結果或任意 crash 不算預期失敗。

## 限制與下一步

- 支援子集維持 [PYSLANG_FRONTEND.md](PYSLANG_FRONTEND.md)；本輪沒有加入 RTL SVA parser，仍須明確拒絕不支援語法。
- 這裡的 properties 是 native C++ monitors，取樣在 NBA 結束後；並非宣稱有完整 SVA preponed/observed scheduler。
- FSM 是 counter 的 ZERO／ACTIVE 觀測模型，不是自動 RTL FSM extraction。功能測試範圍為 counter；不能據此宣稱任意協議的 coverage closure。
- 所有這輪 replay 使用已知 0/1 reset；前一輪 X/Z reset 與四態 mux 測試證據另保留，不能用 Verilator known-value 結果取代四態驗證。
- 下一步優先將 assertion AST、clock／sampling／disable 契約接到這條真實 RTL 路線，並加入多 DUT 的介面描述與未閉合 bin 的選擇策略。
