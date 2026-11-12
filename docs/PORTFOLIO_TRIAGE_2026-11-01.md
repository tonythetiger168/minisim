# Portfolio 回歸跟進：2026-11-01

本輪讀取兩個活躍 controller 的 checkpoint，未重開工作、未修改正在測量的來源。分支維持 `codex/minisim-four-state-mux` 與 Helios 的 `codex/ai-eda-daily-2026-11-01`；既有修改保留。

## 已確認的部分結果

- IP baseline：68/129 完成。core sim 68 pass；syn 66 pass、2 timeout；APB 63 pass、5 timeout。Minisim lowering 68 unsupported，不能將外部模擬通過算成 Minisim 通過。
- VIP basic：130/130 完成，84 整組 pass、46 review。所有 130 的 functional 階段回報 pass，但這不涵蓋尚未執行完的 UVM、coverage 或獨立 crosscheck。
- VIP review 原因是 35 個 smoke skip 與 13 個 elaboration fail，交集 2 個，因此合計 46 個不同 VIP；不是 46 個功能模擬失敗。
- 修正 CRLF 環境問題後，IP coverage checkpoint 已完成 53/258，均回報 closed。這是依上游 waiver 規則的 closure：例如 ACE line 原始 59/61，排除 2；toggle 原始 1122/1128，排除 6。不能稱作未排除的原始 100%。

## 13 個 elaboration fail 的實際診斷

11 個包含缺少外部 DUT module：DDR、GPIO、I2C、I2S、PWM、SD、SPI、UART、USB、eMMC、lpddr5x。錯誤位於 `xchk/*_tb.sv`，例如 `UART_apb`、`DDR4_Lite_top`。這表示 standalone VIP elaboration 的輸入不包含對應 IP，尚不能判定 DUT 或 VIP 動態功能錯誤。

I2S 另外有 4 個 `setup_bad_h` 宣告前使用錯誤，不能因為補入 DUT 就忽略。

HyperBus 與 xSPI 的 monitor `build` virtual method 參數數量與 superclass 不同，須核對實際 UVM API；這是實際語意診斷，不能列為缺 DUT。

證據在相鄰 `helios-verify/build/portfolio-extended-20261101/vip-basic/<name>/elab.log`，完整項目分類在同目錄 `progress.json`。baseline 證據在 `helios-verify/build/portfolio-baseline-20261101/progress.json`。兩者仍會隨續跑增加。

## 下一步與驗收

## 本輪追加根因實測

- HyperBus／xSPI：在容器的臨時副本，將 monitor 自訂 `build(...)` 與呼叫點改名為 `assemble_transaction(...)`，各 2 處；同一 pyslang 12.1／Accellera UVM 環境嚴格 elaboration 均從 **1 error → 0 errors**。因此已重現並定位為與繼承的 UVM `build` 方法名稱衝突。未變更原 ZIP、來源 repo 或活躍 snapshot，也未將此靜態通過當成動態 UVM 通過。重現程式為 Helios `scripts/diagnose_vip_overrides.py`；證據為 extended 結果目錄 `override-diagnostic/report.json`、before/after logs 及修正副本。
- AVSBus smoke 的具體原因：Icarus 回報 `inside expressions not supported yet`，因此 upstream 記為 SKIP。不是 scoreboard 比對失敗；也不能因 functional 階段通過就將這次 smoke 改記 pass。其餘 skip 仍須逐個檢視，不能概括成全部同一原因。
- I2S 的 `setup_bad_h` 第 94–96 行使用，第 123 行才宣告；保留此獨立錯誤，待補齊 IP fileset 後在隔離副本移動宣告並重新驗收。
- HBM4 syn timeout 日誌停在 Yosys techmap；Ethernet_PCS APB 停在 Yosys OPT_MERGE。目前證據是綜合階段超時，不是 assertion／交易失敗；尚未證明只是加長時間即可通過。其他 timeout 仍須逐項確認。
- 本輪發現舊 baseline PID 已不存在，已用既有 lock 與 `--resume --workers 1 --memory-mb 1536` 入口續跑，成功新增 LPDDR 全三階段 pass，checkpoint 至少 **69/129**。extended coverage checkpoint 已到 **96/258**，仍在跑；尚未執行的 UVM 不列 pass。

優先修復順序：先修量測相依/fileset，其次保留已驗證的兩個 helper 改名修正，再處理 I2S 宣告及工具不支援的 smoke backend；綜合 timeout 最後單工分組重測並記錄資源，不直接無限延長所有測試。

1. 保持單工與記憶體限制，先完成兩個 portfolio 全量階段；保留 timeout、skip、waiver、unsupported，不改寫成 pass。
2. 對 11 個缺 DUT 項目，在獨立 crosscheck 階段提供鎖定版本的 IP fileset，再重新 elaboration；I2S 額外宣告錯誤與兩個 UVM override 錯誤分別追蹤。不得直接修改正在跑的 snapshot。
3. 全量結束後再執行已鎖定的 OpenTitan 與四個 FreeCores 專案；目前未新增其動態測試通過證據。
4. Minisim 下一個整合門檻仍是 parameter／hierarchy 等真實 IP 語意；現有有限 VHDL 合成轉譯與 mixed-language 80 筆結果不代表原生 VHDL 或通用 IP 支援。
