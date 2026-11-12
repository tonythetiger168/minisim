# Kernel 執行限制與錯誤契約

2026-11-02，本地 codex/minisim-four-state-mux。這是實驗性 Minisim 的執行保護，尚非完整 SystemVerilog scheduler 或安全 sandbox。

## 可設定預算

在第一次 `S.run(end)` 前設定：

```cpp
S.max_resumes_per_slot = 1000000;
S.max_ir_instructions_per_resume = 1000000;
```

兩者預設均為 1,000,000，必須為正整數。第一項限制同一 simulation timestamp 的 coroutine resume 次數，可中止重複 `#0`；第二項限制 `exec` 每次等待之間的 IR 指令數，可中止沒有等待的跳轉迴圈。它們是計數上限，並非 wall-clock timeout。有限但很大的合法程序也可能達到預算，需依輸入調整並保存設定，不將預算錯誤當作 DUT 功能錯誤。

IR 超限在下一條指令執行與 line coverage hit 前檢查，訊息含 time 與 pc。原生 C++ coroutine 若在一次 resume 內持續執行而不 yield，這兩項上限無法攔截。

## 錯誤與時間邊界

Task promise 保存 coroutine 的 exception_ptr，Sim::run 在 resume 後檢查並以 runtime_error 回報 `process error at time <tick>: <reason>`。標準 exception 保留 what()；非標準 exception 回報 unknown exception。這項更動涵蓋該 Task promise 的程序，不是任意外部 coroutine handle 的介面。

Delay 在將事件加入 wheel 前檢查 `delay > UINT64_MAX - now`，拒絕時間回繞；恰好到達 UINT64_MAX 的合法延遲保留。tick 是目前 kernel 的整數時間，不在錯誤訊息中自行推斷其他時間單位。

程序／預算執行錯誤會設 finished=true 並向呼叫者拋出。剩餘 active／inactive／NBA 不再執行；錯誤發生前已完成的修改不回滾，不能在同一 Sim 上清旗標後當成新執行。該次執行應標記 error，部分 coverage 不代表成功閉合。`run` 前預算為 0 屬設定錯誤，於啟動前拒絕。

## 實測

`python tests/run_core_tests.py`：19/19 通過，包括原有 expression／四態／NBA／finish／coverage/property，以及：

- #0 無限迴圈在 time 0 超限；合法跨時間程序不誤殺。
- 無等待 IR 自跳轉在 time 0 pc 0 超限；wait 會重設 IR 預算。
- coroutine 在 time 1 丟錯後，待執行 NBA 不被套用，外層可捕捉完整訊息。
- UINT64_MAX-1 加 2 被拒絕且沒有插入回繞事件；加 1 正常到達最大時間。

`python tests/compare_counter.py`：11 次數值變動與 t=122 的 finish 和 Icarus 一致。版本 hash 與結果保存 `build/kernel-errors-evidence.json`；這不是任意 RTL、VHDL、SVA 或 UVM 相容性證明。
