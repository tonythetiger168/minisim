# Dynamis TX × Minisim × Helios 整合

使用者明確要求整合 `tonythetiger168/dynamis`，已 clone 至相鄰 `../dynamis`，鎖定 HEAD `2ba99a980913679a2fdd8b156d564c4693e1da9c`、Apache-2.0，分支 `codex/minisim-integration`。讀取 README 與實際 tx/src、tx/tools、tx/ci；沒有找到 AGENTS.md。README 提及的 core/ 在此 checkout 不存在，因此本輪只整合實際可用 TX，不合併或宣稱驗證另一個 simulation engine。

## 實際接線

1. 既有 Python bridge 產生真實 Minisim／Icarus trace，clean／width8／increment2 各 96 samples。
2. `tools/run_dynamis_integration.py` 檢查 bridge kernel SHA、288 samples 證據及每份 trace 的非空／連續 cycle，再轉為 Dynamis TX JSONL。
3. 調用原 repo 的 txdiff.py `--with-time`，保留原始工具、license 與 revision，不複製 comparator。
4. 調用 txview.py，將交易及真實 Minisim VCD 輸出為 HTML。
5. Helios `scripts/run_dynamis_gate.py` 可直接比較 JSONL，輸出 equivalent／different／error 及 SHA、tool revision；不自動生成 calibration label。

這是 trace／TX 工具整合，沒有聲稱 Minisim 支援 DPI-C 的 tx_record，也沒有用 Icarus 輸出冒充 Minisim。

## 驗收結果

```sh
python tools/run_dynamis_integration.py --dynamis ../dynamis
```

三組正常配對全通過；各組插入錯誤值、刪除一筆交易均被拒絕，合計 **9 個比較關卡符合預期**；空輸入另外被 adapter 拒絕。產生 **3 份 transactions.html**（僅驗證產出成功，未宣稱完整視覺 QA）。結果 `build/dynamis-integration/summary.json`，各組保留 minisim/icarus/wrong-value/missing JSONL、diff JSON/log、HTML。

Helios gate 測試 **5/5**：等價返回 0、錯誤值返回 1、缺漏返回 1、空輸入返回 2、輸出路徑與輸入重疊時返回 2 且保留原檔。新增 payload key=value／重複 key 檢查，避免 normalizer 丟棄內容造成空比較。

時間契約為 counter fixture 的邏輯 sample boundary `t=2*cycle+2`，非擷取所有原始 scheduler event；不忽略 jitter。Dynamis 按 kind 分組後依序比較；此例皆為 counter_sample，因此保留 cycle 順序。多 kind 的跨 kind 全域順序不在其比較保證內。increment2 與其自己的 Icarus RTL 配對一致僅證明 backend 一致，不證明 +2 符合 +1 規格；後者另由真正 cocotb／pyuvm scoreboard mutant 測試拒絕。

## 下一步

P0：將真正 cocotb／pyuvm 交易採樣輸出同一 schema，固定 time units 與 DUT/刺激來源 SHA，再接 Helios evidence contract。

P1：加入跨 kind ordering policy、端到端 transaction IDs、未知值與取消交易語意；用至少一個非 counter IP 做同來源刺激差異驗證。不將 TX 等價當完整形式證明，亦不宣稱完整 VCS 替代品。
