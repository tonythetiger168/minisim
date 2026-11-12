# Python 驗證環境

2026-11-01 已在 WSL Ubuntu 的獨立環境 `/home/chienhsinlin/.venvs/ai-eda` 安裝與驗證，未修改系統 Python。Python 版本為 3.14.4。

| 套件 | 已安裝版本 |
|---|---|
| pyslang | 12.1.0 |
| pyuvm | 5.0.0 |
| cocotb | 2.1.0 |

實際驗證：三者匯入成功；pyuvm.uvm_object 建立及名稱讀取成功；pyslang 解析並 elaboration `examples/counter.sv`，取得 counter top，錯誤診斷 0；pip check 通過。

這不等於 Minisim 的 C++ slang lower.hpp 已編譯，亦不代表 pyuvm 已可驅動 Minisim。

同日跟進已完成 pyuvm→cocotb→Icarus 的真實 counter scoreboard：96 個週期、全部 16 個值、3 次 wrap、7 筆 reset 取樣通過；改為加 2 的 mutant 在第 2 筆取樣（7ns）被 scoreboard 判錯。runner 檢查 simulator XML，僅將含 counter mismatch 的預期失敗當作 mutant 成功偵測；缺失、略過或格式錯誤不算通過。來源 RTL 未修改。

重現：在 repo 根目錄使用該獨立環境執行 `python tests/run_pyuvm_counter.py --check-mutant`。建置與 XML 暫存目錄會在完成後清除。嵌入 simulator 的 libpython 與 venv 的 Python 小版本可能不同；本次實際模擬已通過，但不推論其他安裝環境也相容。

Windows PowerShell 使用既有環境：

```powershell
wsl -d Ubuntu -- /home/chienhsinlin/.venvs/ai-eda/bin/python -m pip check
```

在 Linux／WSL 重建：

```sh
python3 -m venv ~/.venvs/ai-eda
~/.venvs/ai-eda/bin/python -m pip install -r requirements-verification.txt
```

requirements-verification.txt 固定三個直接依賴；尚非包含所有間接依賴與 hash 的完整鎖定檔。

官方套件來源：[pyslang](https://pypi.org/project/pyslang/)、[pyuvm](https://pypi.org/project/pyuvm/)。

本聊天排程已改為每小時跟進並回報，優先深化 Minisim；每日全球 AI EDA 研究仍保留，修改與整合測試依實際證據記錄。
