# 全量 portfolio／OpenCores 與混合語言驗證計劃

使用者新增授權（2026-11-01）：先跑完 open-ip-portfolio、open-vip-portfolio，再加 GitHub／OpenCores 的 UART、SPI、I2C、GPIO 等代表性 IP。另要求 Minisim 支援 VHDL 及 Verilog/SystemVerilog 混合語言模擬；此要求加入後續實作，不取代已開始的全量回歸。

## 已確認範圍與執行狀態

- open-ip-portfolio：HEAD e6a39a36d131c916688735a9874dfafb30474203，共 129 個 Makefile/IP。
- open-vip-portfolio：HEAD ba3cf117a2d18713df5c6562d3428afa896a8a94，共 130 個 ZIP 封裝；README 開頭的 118 是較舊描述，以實際檔案／manifest 計數為準。
- 已開始 129 IP 的 core sim／syn／APB 全量回歸。每個 IP 在隔離 Linux snapshot 下執行，原始 repo 不寫入 simv 或 work。各命令有 timeout 並保留退出碼與 log。
- 同時逐項嘗試 Minisim lowering；unsupported、lowered-only、實際 simulation pass 分開分類。不能把外部 Icarus 的 pass 標為 Minisim pass。
- 基本回歸程式：相鄰 helios-verify/scripts/run_portfolio_baseline.py；輸出 helios-verify/build/portfolio-baseline-20261101/progress.json、完成後 report.json。若已在跑，先查進度，不要重複啟動。
- 系統 Verilator 5.032 不符合 portfolio 五類 coverage 要求（>=5.052）。已取得官方 v5.052 Docker image，準備獨立 runtime，加入 pyslang、UVM 2020.3.1、Icarus、Yosys、GHDL 及 Z3。這不是將 runtime 安裝成功宣稱為 coverage/UVM 通過。

## 預定驗收階段

最新使用者新增 Basanos 整合期間：host 可用實體記憶體約 560 MB，且有其他專案容器活動；只停止本輪 `ai-eda-portfolio-20261101` 釋放資源，停止已確認完成，保留其 snapshot/progress。其他專案不動，沒有重啟全 WSL。後續先核對 host 資源與 Basanos 工作再恢復原 container，不能依舊段落誤認它仍在跑，也不建立重複全量工作。Basanos 當前已修 lint API／ConfigDB 相容並取得 Icarus pyuvm 500 筆0錯，跨 repo gate 與FSM coverage結果待驗收；細節以 Basanos integration 文件為準。

2026-11-02 13:01 UTC 最新：UART lowmem 實驗已動態通過，88 transactions／88 scoreboard matches、0 mismatches、0 UVM errors/fatals、0 OOM kill。證據在 Helios build/uvm-lowmem-20261102/acceptance.json 與 resume-run.log；僅單一 smoke，非 Minisim UVM 支援。相同 compile profile 已加入 extended runner，原 `ai-eda-portfolio-20261101` 已恢復，繼續 VIP coverage／independent crosscheck。舊 UVM fail 結果仍保留，不改記 pass；不要重啟已完成的 UART 容器或另開全量 job。

2026-11-02 12:00 UTC：UART 容器原生第一段續建 119→1611 objects，30 分鐘 timeout、無 OOM kill，尚無動態結果。已歸檔 resume-report-segment1.json 並啟動相同 resume container 保留快取；不要啟動原 lowmem 或全量 extended。第二段啟動時需先核對 fingerprint，report 尚未刷新不代表已完成；以 container/process 與新 checkpoint 判斷。Helios runner 新增歷史 report／segment 與原子寫檔，下一次啟動生效。

2026-11-02 11:04 UTC：原 UART lowmem build 30 分鐘 timeout，OOM delta 0，未執行測試。現在唯一活躍入口是 `ai-eda-uart-resume-20261102`，重用既有 generated source／119 objects，移到容器原生 cache 續建；report 為 Helios build/uvm-lowmem-20261102/resume-report.json。先查此容器，不重啟原 lowmem（其 upstream build 會刪掉已編譯產物），也不重啟 extended 全量。詳見同日研究報告 11:04 UTC。

2026-11-02 10:02 UTC 最新入口變更：因 host 記憶體壓力與 OOM kill 持續增加，原 extended container 已停止保留；目前只執行 `ai-eda-uvm-lowmem-20261102` 的單一 UART smoke 分割／-O0 編譯實驗，結果在 Helios build/uvm-lowmem-20261102。先讀此 report/flow/container 狀態，不重複啟動全量。低記憶體實驗完成後才選擇恢復原 extended 工作或套用已驗證設定；目前 VIP coverage 未完成。詳見 Helios 同日日報 10:02 UTC 段。

2026-11-02 08:57 UTC：IP baseline 全 129 已完成並有 final report.json；sim 128 pass/1 timeout，syn 126 pass/2 timeout/1 resource_limited，APB 123 pass/5 timeout/1 resource_limited。Minisim 129 unsupported。VIP coverage 繼續執行，容器 cgroup 已記錄本次有 2 次 OOM kill；先處理資源與錯誤分類，再做有意義的單一 UVM 重測，避免把全部 attempted 說成通過。FPGA研究新證據與限制同步寫入 Helios docs/research/2026-11-02.md。

2026-11-02 增量：coverage 已 attempted 258/258（257 closed、HDCP 1 build failure），UVM 已 attempted 130/130（原始 121 fail、7 timeout、2 no_test_or_result）；尚未等於動態測試通過。保存 log 分類器確認共 119 項含 compiler SIGKILL，資源壓力疑似但尚無 OOM counters 證明。詳見 Helios docs/research/2026-11-02.md 與 build/portfolio-extended-20261101/failure-classification.json。Docker 中斷後已恢復本輪 container，baseline 停在 83/129 後已呼叫原 resume；等待新增 checkpoint 才認定續跑有新成果。新增 FPGA 資源／bitstream 需求沿用 [FPGA_TOOLS.md](FPGA_TOOLS.md)，不得遺漏。

執行更新：VIP basic 已嘗試全 130（84 整組通過、46 需檢視各階段；不是 46 個動態功能失敗）。原 258 個 coverage attempt 被 Makefile 的 CRLF 污染，已整批標記 invalid、歸檔至 ip-coverage-invalid-crlf，不計作 IP 失敗；在 snapshot 正規化後已開始逐項重跑並取得 CLOSED。大量工作亦造成暫存空間／WSL/Docker 服務中斷，已備份 progress、清掉本輪中斷的生成暫存，加入單工、記憶體上限、process lock 與 checkpoint resume。原始 repo 未清理或修改。

本輪活躍入口：

- 基本回歸：`helios-verify/build/portfolio-baseline-20261101/controller.json` 記錄目前 controller；`progress.json` 是部分結果、`report.json` 僅在全部完成後產生。續跑命令加 `--workers 1 --memory-mb 1536 --resume`，不要另開重複 job。暫存改放 Linux disk cache，每個 case 結束即移除生成物，完整 logs 保留。
- extended：Docker container `ai-eda-portfolio-20261101`，image `ai-eda-portfolio:20261101`，CPU=2、memory=2304m、memory+swap=3072m；程式 `helios-verify/scripts/run_portfolio_extended.py`。來源是 read-only mount，實際工作在容器 `/work`，結果 bind 至 `helios-verify/build/portfolio-extended-20261101/`。
- `progress.json` 保留每 phase／item；已完成 phase 續跑會跳過，未完成 phase 依 completed names 續跑。container 的 source snapshot 已存在時不可刪掉重做；新的工作版本已支援從保存進度復原。
- 避免同時高並行的綜合與 UVM build。服務卡住時先保存結果、僅處理本輪自己的 controller/container；不擅自重啟所有 WSL 或中止其他專案。

1. 全 129 IP：core sim、core syn、APB sim+syn；報告 pass／fail／timeout 及逐項 logs。
2. 全 258 core+APB checker：使用上游 scripts/cov/run_cov.sh，保留 line／FSM／toggle／SVA／functional 的原始分母、命中、排除項及 holes；不得只報 CLOSED 百分比而省略 waiver。
3. 全 130 VIP：封裝解開、pyslang elaboration、smoke、所有可發現 functional tests、已定義 UVM suites／其餘 UVM smoke；沒有 test 或僅 elaboration 不當作動態驗證通過。
4. 全部上游獨立 IP crosschecks（目前定義 16 類別），不能以原 DUT 產生的值冒充外部 golden。
5. 再引入代表性 GitHub/OpenCores IP；鎖定 repo、revision、license 與 upstream testbench，優先 UART/SPI/I2C/GPIO，避免任意挑幾個後宣稱掃遍整個 GitHub/OpenCores。

## VHDL／mixed-language 路線

- Minisim 目前原生只執行已驗收的有限 SV 子集，**尚無原生 VHDL 或完整 mixed-language 支援**。
- 先用 GHDL 建立 VHDL-2008、reset、delta cycle 與 signal assignment 的參考案例；VHDL 九值 std_logic 與 Minisim 四態之間需明確映射，不能靜默合併強弱／don't-care 語意。
- 定義跨語言 port/type/width、time resolution、delta cycle、driver resolution、clock/sample 與事件順序契約；再做 SV→VHDL 及 VHDL→SV 的雙向 DUT/TB 案例。
- GHDL synthesis 轉出的 Verilog 可作為受限 synthesizable 前端路線，但須標示為轉譯；不能因此宣稱支援任意 VHDL testbench 或原生共用事件核心。
- 後續先完成一個真正可重現的 mixed-language reference，再接 Minisim kernel；保留不支援項，不能把外部 co-simulation 說成 Minisim 原生完成。

所有工作保留在本地 codex 分支；不自行 push、merge、發布或啟用付費工具。


## 2026-11-03 15:20 Taipei：VIP coverage checkpoint缺口

實際讀取停止容器snapshot tools/coverage.py，發現雖支援指定VIP局部執行／合併舊CSV，卻只在整批Pool結束才寫報表。既有log有4個COV結果，但容器coverage_report.csv不存在，不能視為4個持久checkpoint。新增Helios scripts/patch_vip_coverage_checkpoint.py，在每個completed row以fsync+atomic replace合併保存；明確保留covered_tests/detail而非全部標PASS。

暫存測試3個情境通過：第一項中斷後可讀、後續續跑合併、同名更新不重複；亦驗證patch idempotence與實際上游source可compile。原/修改snapshot保存在build/vip-coverage-checkpoint，修正已copy回本輪停止容器的private snapshot，未改open-vip-portfolio原repo、未啟動容器。舊4項log無CSV不偽造補記；下一步可核對原始dat/log後受控重試單一VIP，不能宣稱已恢復coverage。主機可用449764KiB約439MiB，未啟動大編譯、不動其他專案。未推送。


## 2026-11-03 16:50 Taipei：搶救已存在coverage raw data

停止容器的四個merged.dat（ACE-UVM-VIP、ACE_Lite-UVM-VIP、AHB-VIP、APB-VIP）均成功只讀取出，已保存build/portfolio-extended-20261101/recovered-coverage，manifest記錄大小及SHA。它們對應早前四項COV log；UVM build皆曾exit124，不標完整PASS，也未偽造CSV。這些raw data可供後續受控verilator_coverage解析，避免為取得同一份資料重編譯；仍需保留functional tests與UVM各自結果與分母。

本輪新增模擬0，可用664624KiB約649MiB，原容器仍exited。下一步以5.052相容parser／官方工具驗證原始檔，僅重建有依據的部分coverage報告；完整VIP coverage仍未完成。OpenTitan/OpenCores順序保持，未推送。


## 2026-11-03 17:20 Taipei：四份coverage部分報告恢復

用保存的原上游coverage.py parser/summarize與停止容器原flatten manifest（僅FLAT DUT來源）重新讀merged.dat，未重新編譯。四個結果line/toggle百分比與舊log逐項一致：ACE line112/121、toggle1001/1552、expr86/94；ACE Lite line11/11、toggle183/2282、expr17/18；AHB line13/13、toggle364/646、expr15/15；APB line19/19、toggle196/518、expr19/19。SVA/user與func皆0/0，明確未量測，不算closure。

新增scripts/recover_vip_coverage_report.py，保存parser SHA、raw/manifest SHA、DUT files與line holes；partial-report.json在build/portfolio-extended-20261101/recovered-coverage。這是functional-test部分coverage復原，四個UVM build timeout狀態保持，不偽造上游完整CSV/checkpoint或overall PASS。parser沿用5.052語意（line含當時branch），不得與5.054分離旗標直接混比。原始資料可能含coverage-build失敗樣本，僅表示點曾被執行，不能取代正確性證據。下一步以保留報告安排未完成／timeout重試及真正checkpoint選擇；本輪模擬0，未推送。


## 2026-11-03 18:20 Taipei：只讀VIP coverage續跑清單

新增scripts/plan_vip_coverage_resume.py，對照實際130個ZIP與恢復的partial-report：4項部分coverage、126項尚無保存coverage紀錄；此phase完整驗收0，不與前述basic/functional/UVM attempted數混淆。4項UVM timeout仍列retry_required，130項皆pending_full_acceptance，不因raw data存在跳過。清單本身不啟動job，保存輸入report SHA；非完整來源簽章。

4檢查通過：partial保持pending、重複VIP拒絕、未知VIP拒絕、錯誤分母拒絕。實際清單在build/portfolio-extended-20261101/vip-coverage-resume-plan.json。容器exited、可用1233700KiB約1.18GiB，未啟動編譯，本輪新增模擬0。下一步可依清單受控單項執行並驗證新增CSV，不能把清單產生稱成回歸完成。未推送。


## 2026-11-05 00:02 Taipei：CSV結尾原子寫入修正

檢查受控單VIP入口時發現V1 patch僅逐項checkpoint原子寫入，main結尾仍open(REPORT,w)截斷重寫；若此階段中斷，可能破壞先前CSV。Helios scripts/patch_vip_coverage_checkpoint.py升級V2，結尾亦呼叫checkpoint_rows，支援既有V1升級。新增scripts/test_vip_checkpoint_finalization.py，首次/升級內容一致、idempotence、結尾呼叫檢查、replace故障保留舊CSV並恢復合併，4/4通過；git diff --check通過。

本輪沒有啟動容器或RTL模擬，沒有宣稱實際VIP落盤驗收。V2尚未copy到停止容器；容器仍exited、內部仍為先前V1，下次受控執行前必須升級並核對SHA。host可用1686208KiB約1.61GiB，不以container上限當最低需求。保留全部部分coverage／timeout分類，未推送。

P0下一步：部署V2至private snapshot並回讀校驗，安全建立受控單VIP入口，驗收真實CSV及程序狀態；效益高、成本中、資源風險中。Minisim共用build與6項故障注入沿用前輪有效證據，無變更未重跑；本輪新增模擬0。


## 2026-11-05 00:32 Taipei：停止容器V2部署驗證

將目前container private snapshot的V1 coverage.py先取出保存，再以patched()升級V2，copy回同一路徑並再次讀回；原上游repo未改。證據build/vip-coverage-checkpoint/deploy-v2-20261105/deployment.json及before.py/coverage.py/readback.py。回讀SHA與部署檔完全相同fbee3655c9f559d6efdebe21b389304244a2d18c4be8a6dd3baf9e909dd60a49，語法compile通過且V2 marker存在；前SHA32e6cbf4c53a5e6f9e1811400b6619f09adc37d6498f0d865e4af81a82e3de2c。

容器前後皆exited，沒有start controller或重跑先前PASS；新增RTL／VIP模擬0。這是部署內容校驗，不是真實單VIP CSV落盤驗收，後者仍待受控入口。沿用前輪4/4測試，無程式變更不重跑；Minisim build6/6與Icarus既有成果未擴大宣告。下一步建立安全單VIP命令入口、檢查資源並保留timeout／partial分類。未push／merge／啟動付費API。


## 2026-11-05 01:02 Taipei：獨立單VIP入口

確認停止容器的Config.Cmd直接啟動整批controller，不可直接docker start。新增scripts/run_single_vip_coverage.py獨立入口，明確request只允許vip與timeout_seconds、一個安全名稱、1..3600秒及V2 marker；執行child覆寫REPORT到全新exclusive output目錄，固定-j1，保存request/sourceSHA/log/returncode/timeout，不動原progress.json。此入口尚未接到容器啟動命令；不能宣稱已能直接start原容器執行單項。

scripts/test_single_vip_request.py的一個測試方法內8項契約情境全部符合預期（正常1，空名／旗標／路徑／多名4拒絕，bool／過大timeout2拒絕，舊V1拒絕1）。非真正VIP模擬，新增模擬0；diff check通過。host可用1714196KiB約1.63GiB，容器仍exited。下一步驗證child執行／CSV保存與timeout流程，建立安全啟動dispatch後才做實際單VIP；不改上游、不推送。


## 2026-11-05 01:32 Taipei：單VIP實際子程序流程測試

新增scripts/test_single_vip_execution.py，WSL既有venv真實啟動輕量coverage fixture子程序，非mock subprocess；成功退出0、失敗退出7、逾時kill三項3/3通過（1.056秒）。各項均驗證CSV標題與資料列、execution.json、log、已存在output拒絕且不覆蓋原CSV。逾時前寫入的checkpoint保持可讀；此為fixture資料，非真正VIP coverage或RTL PASS。

修正run_single_vip_coverage.py CLI原先只print且總以0退出：現在timeout回124、其他子程序失敗回1、成功回0；execution.json保留原returncode。上述三項直接驗證execute()，CLI狀態映射尚未獨立動態驗收，下一輪應補驗再接實際啟動。移除無用imports；diff check通過。

Portfolio容器仍exited，未改其Config.Cmd、未啟動整批，沒有真實VIP/RTL新增模擬。Minisim既有共用build與Icarus證據沿用，不冒稱本輪重驗。P0下一步：CLI狀態與安全啟動dispatch，再受控單VIP落盤；未推送。


## 2026-11-05 02:02 Taipei：CLI退出碼動態驗收

擴充scripts/test_single_vip_execution.py，新增test_cli_exit_codes，真正以sys.executable啟動run_single_vip_coverage.py命令列，再由其啟動coverage fixture。只跑新測試，3個subTest全通過：正常CLI0、child7映射CLI1且JSON仍記7、逾時CLI124且timeout=true；各項stdout JSON與execution.json相同且CSV兩列可解析。測試耗時1.443秒，未無故重跑上一輪3項execute測試，diff check通過。

這是輕量流程fixture、非RTL/VIP模擬，本輪新增真實VIP模擬0。容器仍exited，啟動命令仍整批controller；單項入口尚未接啟動dispatch，不可直接start。下一P0受控dispatch需明確單VIP請求、互斥與原progress.json保留，之後才能實際單項CSV驗收。分支codex/ai-eda-daily-2026-11-01，保留既有修改，未推送／合併。


## 2026-11-05 02:32 Taipei：單VIP啟動分流與重播保護

run_portfolio_extended.py啟動時若/results/single-vip-request.json或single-vip-request.claimed存在，僅走單項dispatch；分流在batch錯誤handler外，失敗不寫原progress.json也不退回整批。run_single_vip_coverage.dispatch驗證request/V2後以exclusive create建立持久claim，再執行固定single-vip-coverage獨立目錄；claim不自動刪除，避免restart重播。只刪request仍有claim時亦不會退回batch，缺request會明確失敗。

新增test_single_vip_dispatch.py兩項2/2通過：重複dispatch拒絕且execute只呼叫一次；execute異常仍保留claim，兩者原progress bytes不變。這兩項是mock execute的分流測試，不是新的VIP模擬；容器啟動端到端仍待測。既有三情境真實CLI測試未因未改CLI而重跑。diff check通過。

主機可用441008KiB約431MiB，未啟動大型編譯。容器仍exited、restart policy no；本輪沒有建立正式request或claim，沒有start容器。新分流程式位於既有/runner唯讀bind的host scripts，下次啟動可見，但必須先完成端到端驗證與資源檢查。下一步在fixture子程序驗證controller分流，再資源允許時建立明確單VIP請求；未推送。


## 2026-11-05 03:02 Taipei：controller啟動分流fixture驗收

新增scripts/test_single_vip_controller.py，讀取實際controller AST，僅將OUT/VIP常數重定位臨時fixture並把整批main替換成明確失敗哨兵；執行原啟動分流區塊、實際dispatch/execute/child。一個test內三次真實controller子程序符合預期：初次成功且CSV/JSON存在；第二次FileExistsError拒絕重播；移除request後第三次FileNotFoundError拒絕，皆未落入整批main。原progress bytes及首次execution JSON完整保留。測試0.525秒，diff check通過。

此為路徑重定位fixture流程驗收，不是Docker端到端或VIP/RTL模擬；本輪新增VIP模擬0。主機可用678544KiB約663MiB，容器仍exited，未建立正式request/claim或啟動大型編譯。下一步資源允許時確認VIP實際名稱與工具，再建立正式單項request及保存progress SHA，受控start只走單項；狀態/分母不預先標PASS。未推送。


## 2026-11-05 03:32 Taipei：UART真實名稱與來源核實

實際archive為open-vip-portfolio/vips/UART-VIP.zip，不是先前契約測試任意使用的UART-UVM-VIP字串；正式request必須UART-VIP。保存build/portfolio-extended-20261101/uart-single-intake.json，含archive SHA、31來源項及4個候選test檔；盤點非模擬。上游docs/test_suites/UART-VIP.md的61checks及18/18 mutation是既有上游報告，沒有本輪重驗；其商用UVM描述也不能覆蓋本地既有88交易低記憶體證據，更不能外推全量coverage。

本輪host可用763152KiB約745MiB，容器仍exited，沒有建立正式request或開始編譯，新增模擬0。未製造無關程式修改。下一步先核對private snapshot同名目錄與低記憶體profile，再資源允許時執行單項；保留分母與timeout。

研究：arXiv英文RTL+日期與OSCC/AiEDA release搜尋未取得可核實24h新增發布，回傳包括right-to-left縮寫誤命中及聚合日期，均未計新聞。官方來源https://github.com/OSCC-Project/AiEDA與https://ieda.oscc.cc/res/papers/25-ISEDA25__AiEDA_2_0.pdf仍屬既有基線，沒有宣稱本地整合或新版本驗證。未推送。


## 2026-11-05 04:02 Taipei：真實Pool派工缺口重現與修正

新增test_fork_pool_module_pickling，以真正Linux fork Pool(1)/imap派送動態coverage模組的worker。修正前明確失敗：ModuleNotFoundError single_coverage及PicklingError；先前不含Pool的fixture不能覆蓋此問題。run_single_vip_coverage.py現在於exec_module前註冊sys.modules[spec.name]=m，fork worker繼承模組身分。修正後整份test_single_vip_execution.py五個測試方法5/5通過（2.678秒，含CLI三種subTest），diff check通過。

這是fork/imap與流程fixture，尚未在容器Python版本跑真正coverage.py，也未宣稱spawn/forkserver通用支援。真實VIP/RTL新增模擬0。主機可用870380KiB約850MiB，原容器仍exited，未start或建立正式request。下一步核對容器Python與上游Pool start method，再受控單項；不因流程5/5就宣稱VIP coverage完成。未推送。


## 2026-11-05 04:32 Taipei：實際Portfolio image流程驗證

以既有本地ai-eda-portfolio:20261101、--pull never/--network none/128MiB/1CPU啟動兩個短生命探測容器（完成後--rm），未啟動原controller。實際Python3.12.3 GCC13.3.0，預設multiprocessing start method fork（可用fork/spawn/forkserver），verilator=/usr/local/bin/verilator，iverilog=/usr/bin/iverilog。

在同image中scripts唯讀bind，真實Pool pickling及controller三次啟動分流兩項測試2/2通過，0.395秒；這補足先前僅WSL Python3.14驗證的環境差異。證據build/portfolio-extended-20261101/runtime-probe-20261105.json。仍是fixture流程，不是真正VIP coverage；原private snapshot沒有被這些短生命container掛載或修改。

開始前host可用591076KiB約577MiB，選128MiB短程序成功，沒有把2304MiB cap當最低需求；原容器前後exited，未動其他專案。新增RTL模擬0，未推送。下一P0資源允許時核對private UART來源與低記憶體profile、正式單項request／原progress SHA後受控試跑；不外推全量完成。


## 2026-11-05 05:02 Taipei：單項分流低記憶體profile缺口修正

讀取停止容器runner確認真正VIP解包預設路徑為build/vips，先前讀/work/open-vip-portfolio/vips/UART-VIP失敗屬路徑錯誤，不是UART來源不存在。container Config.Env為UVM_JOBS=2及普通PATH；原低記憶體wrapper設定在batch main內，新單項dispatch會繞過，因此不能沿用舊描述宣稱單項已受保護。

run_single_vip_coverage.execute現在於獨立output/runtime-bin建立原已驗證同旗標wrapper：output-groups0/split3000/cfuncs500/O0/makeOPT三項O0/-j1，明確child env UVM_JOBS1、VERILATOR及PATH，compile-profile.json記profile及wrapper SHA。不改上游或全域環境。真實child fixture加檢查三個環境條件，受影響子程序/CLI/Pool五測試5/5通過（2.579秒），diff check通過。仍不是實際Verilator編譯，不能稱記憶體需求已量測。

開始前可用565952KiB約553MiB，原container仍exited，未執行正式VIP，新增模擬0。下一步核對build/vips/UART-VIP來源並在資源允許時單項試跑；本輪修正的是啟動前實際發現的profile遺漏。未推送。


## 2026-11-05 05:32 Taipei：真正UART單項CSV落盤驗收

開始前host可用1499392KiB約1.43GiB。從停止容器正確build/vips/UART-VIP取出31來源檔，與上游ZIP逐檔LF-normalized完全相同（source-comparison.json）。保存原progress SHA後建立正式唯一request UART-VIP/180秒，經已測dispatch受控start原container；固定單工split/O0 profile生效，沒有跑batch。

真正上游Verilator coverage完成5.5秒，container退出0、timeout=false，CSV僅UART一列：tests1/covered1，line18/20、directional toggle9/10、expr4/4；line holes dut_uart_loopback_model.sv:21,49，SVA/user assert與func均0/0未量測，不能宣稱closure或完整UVM。這不是Minisim原生PASS。原progress.json SHA完全不變，未改寫歷史phase結果。

證據single-vip-coverage/coverage_report.csv、execution.json、compile-profile.json、flow.log；原始coverage檔及logs取出uart-preflight/coverage-evidence，acceptance.json記SHA。正式request與持久claim保留，重啟會拒絕重播，不可直接再start。原container現已exited；沒碰其他專案。下一步將新CSV與recovered partial清單正確整合，保留UART coverage holes與0/0，規劃下一個尚未完成VIP；全130 coverage仍未完成。未推送。


## 2026-11-05 06:02 Taipei：UART checkpoint整合續跑清單

plan_vip_coverage_resume.py新增可選--checkpoint，讀取實際CSV並驗證名稱唯一、tests正分母、各metric hit<=total、line非空；保留line holes/detail及每metric measured旗標。新schema v2區分checkpoint_recorded_pending_full_acceptance，仍fully_completed0、pending_full_acceptance全130，不將covered_tests1/1當UVM/coverage全通過。舊v1無checkpoint入口保留。

真實輸出vip-coverage-resume-plan-v2.json：130 inventory、4 recovered partial、1 UART CSV checkpoint、125尚無保存coverage紀錄，4項舊UVM timeout仍保留。test_vip_checkpoint_plan.py四項4/4通過：pending/0分母未量測/holes保留、invalid count、duplicate、empty拒絕；diff check通過。沒有重跑UART或改歷史progress。

原container仍exited、正式request/claim保留防重播，本輪新增模擬0。下一步按清單選未完成VIP、設計保存舊single output及重置claim的明確新attempt流程，不能刪證據後盲目start；coverage全量仍未完成，未推送。


## 2026-11-05 06:32 Taipei：SPI coverage後端風險盤點

下一候選實際為SPI-VIP.zip，與eSPI/xSPI分開。讀取真正tb/smoke_func_tb.sv，包含MISO idle釋放為Z及四態比較；上游docs報告Icarus10checks/25mutants不等於Verilator coverage實證。保存spi-coverage-preflight.json含archive/test SHA與四態比較精確行號；沒有改測試、排除Z條件或宣稱Verilator已失敗（本輪未跑）。

host可用626716KiB約612MiB，container exited，UART request/claim/CSV原位完整保留，未為換下一項清掉證據。新增模擬0，未製造無關程式修改。下一步資源允許時以原test受控驗證後端，若不支援明確unsupported/waiver而非改test假PASS；亦可選不含三態的未量測VIP先推進。全130coverage未完成，未推送。


## 2026-11-05 07:02 Taipei：獨立named attempt保留舊成果

run_single_vip_coverage.py接受可選attempt_id（1..64安全英數/_/-，拒絕路徑），dispatch為新attempt使用single-vip-attempts/<id>/獨立claim與single-vip-coverage。既有UART根目錄claim/output完全不移動、不刪除；相同id重播仍拒絕。controller見attempts目錄亦保持單項分流，request遺失時明確失敗不退回batch。舊request格式相容。

test_single_vip_dispatch.py四項4/4通過，新增named output路徑正確/legacy evidence與claim bytes保持、相同id拒絕、../escape在建立目錄前拒絕；原失敗/重播測試亦回歸。diff check通過。這是mock execute的dispatch測試，named attempt實際child/controller仍待補驗；不能直接稱可完整續跑。

host可用505056KiB約493MiB，原container exited，正式UART request未改，本輪真實VIP模擬0。下一步驗證named attempt的實際子程序後，資源允許時用新id跑SPI或其他未量測項；不清除歷史證據。未推送。


## 2026-11-05 07:32 Taipei：named attempt容器image流程驗收

擴充test_single_vip_controller.py共用exercise，新增named_start_replay_and_orphan_claim。於實際Portfolio image Python3.12/fork，128MiB/1CPU/無網路/scripts唯讀短生命container執行新測試：三次真實controller子程序符合預期，named apb-001初次寫獨立CSV/JSON、同名重播FileExistsError、移除request但attempt目錄存在時FileNotFoundError，皆不進batch且progress與首次成果bytes不變。特別確認不存在legacy全域claim，防護真正依named目錄生效。

新測試1方法3次啟動通過，0.438秒；沒有重跑舊legacy測試，diff check通過。這是實際image內路徑重定位fixture，不是正式VIP/RTL模擬，新增模擬0。原container仍exited，正式UARTrequest/claim/output完整未動。下一步資源允許時建立新attempt正式request並保留原request副本，受控SPI或其他未量測VIP；不可外推全130完成。未推送。


## 2026-11-05 08:02 Taipei：SPI真正named單項完成

開始前可用1519556KiB約1.45GiB，private SPI-VIP31來源檔與ZIP LF-normalized全相同。備份原UARTrequest及progress/UART CSV SHA後，以attempt_id spi-20261105-001、180秒、單工split/O0受控start，沒有刪除legacy claim/output。5.3秒container退出0、timeout=false。

真正Verilator5.052原測試log FUNC_PASS checks=10、t=8us；之前三態風險僅評估，此次原測試在此後端實測通過，不可繼續稱SPI不支援。CSV tests1/covered1，line22/25、toggle46/50、expr10/13；line holes43/46/64，SVA/func均0/0未量測，非完整closure／全UVM／Minisim PASS。原progress及UART CSV SHA均不變。

證據single-vip-attempts/spi-20261105-001/single-vip-coverage，及uart-preflight/spi-coverage-evidence原dat/log、spi-acceptance.json。container已exited，named claim保持防重播；下一步清單整合多checkpoint（UART與SPI），全130未完成。未推送。


## 2026-11-05 08:32 Taipei：UART/SPI多checkpoint整合

plan_vip_coverage_resume.py --checkpoint可重複指定，各來源記path/SHA；單一Path呼叫仍相容。多CSV合併時同名VIP拒絕而非silent overwrite，空來源列表／空CSV拒絕。test_vip_checkpoint_plan.py五項5/5通過（新增多來源及衝突/空列表情境），diff check通過。

實際輸出vip-coverage-resume-plan-uart-spi.json：130 inventory、4 recovered partial、UART/SPI兩個checkpoint、124尚無保存coverage紀錄；fully_completed仍0、pending_full_acceptance全130、4項舊UVM timeout不變。兩份獨立run各保留原分母、holes、assert/func0/0未量測，不合成全域時序或宣稱全量PASS。

本輪新增模擬0，未重跑UART/SPI，container仍exited、named claim保留防重播。下一步按清單選下一個未量測VIP受控單項；Minisim共用build與VPI/Icarus既有成果不重跑不擴大宣告。未推送。


## 2026-11-05 09:02 Taipei：I2C來源核對，未新增模擬

確認private build/vips/I2C-VIP與正式ZIP共31檔LF-normalized完全一致，i2c-coverage-preflight.json保存archive SHA/逐檔比對/四態條件精確行號。上游Icarus15checks與24mutants屬舊報告，不冒稱本輪結果。

host可用576160KiB約563MiB，原container exited，正式request仍為SPI已完成attempt，UART/SPI與progress全保留。這輪未啟動編譯／新request，新增模擬0、程式修改0，未為湊進度製造測試。下一步資源可行時用新i2c attempt受控執行；不能以container2304MiB上限當最低需求，需按單項實際資源評估。全coverage清單仍4partial/2checkpoint/124無紀錄、完整0。未推送。


## 2026-11-05 09:32 Taipei：I2C受限資源真正單項完成

host可用709252KiB約693MiB，非以2304MiB cap當最低需求。保存原容器Memory/MemorySwap後，僅本輪容器暫設512MiB RAM、768MiB含swap，單工split/O0、named i2c-20261105-001/180秒執行原I2C測試。7.88秒正常退出0、timeout=false、OOMKilled=false；完成後原容器limits已恢復，沒有動其他專案。上限不是peak RSS測量，也不能外推其他UVM需求。

真正Verilator5.052 log FUNC_PASS checks=15，t67us；CSV tests1/covered1，line21/22、toggle24/2080（1.2%，仍有大量holes）、expr9/10，line hole dut_i2c_slave_model.sv:60，SVA/func0/0未量測，非closure或Minisim PASS。原progress、UART與SPI CSV SHA皆不變。

證據single-vip-attempts/i2c-20261105-001/single-vip-coverage與uart-preflight/i2c-coverage-evidence、i2c-acceptance.json、資源原設定保存。container已exited，claim保留。下一步把I2C新checkpoint加清單並依實際holes設計刺激；全130仍未完成，未推送。


## 2026-11-05 10:02 Taipei：I2C toggle holes有據分類

使用保存同版coverage parser讀取原merged.dat，依o訊號群組統計並assert總數2080/命中24與CSV完全一致。i2c-toggle-holes.json保存raw/parser SHA、群組與未命中例：mem12/2048、cur_addr4/14、bit_cnt0/8、restart0/2，其餘四個port各2/2。沒有修改分母或waiver。

再讀真正flatten DUT：mem[0:127]共128x8bits，但mem讀寫位於cur_addr==SLAVE_ADDR區塊（原source行77/81/96），不能直接設計掃128地址並宣稱必可closure；其他地址候選不可達需獨立證據及明確waiver流程。bit_cnt只有宣告無使用，8個direction bins候選靜態死點；restart有實際assignment，應以repeated-start測試驗證，尚未補測。優先可行刺激為有效slave地址資料00/FF切換及repeated-start，驗收需正確scoreboard與新增bins，不以硬排除拉高百分比。

I2C已加入三CSV規劃器，vip-coverage-resume-plan-uart-spi-i2c.json：130總項、4partial、3checkpoint、123無保存coverage、完整0。新增模擬0，沒有重跑既有smoke；container exited，未推送。下一步先完成其餘VIP一次回歸順序，coverage定向補測設計保留待辦。


## 2026-11-05 10:32 Taipei：未量測VIP來源清單與GPIO核對

可用358824KiB約350MiB，未啟動編譯。先前口頭GPIO-VIP為候選猜名，實際archive與private來源為GPIO-UVM-VIP，已更正路徑並驗證27來源檔與ZIP LF-normalized一致。上游42checks/1mutant是既有報告，不當本輪驗收。

新增pending-vip-source-inventory.json，按真正三checkpoint規劃的123項無紀錄VIP逐一保存archive SHA、檔案數、解壓大小及候選functional TB清單；這是機器可讀盤點，不是全部attempted或PASS，大小也不是最低RAM。可供後續正確選名與確認test入口，避免每輪猜名或掃遍上游無據宣稱。

本輪新增模擬0／程式修改0，UART/SPI/I2C成果與進度未動、container exited。下一步資源可行時正式named gpio attempt；若低資源持續則回到Minisim可低資源實作，不僅輪詢。未推送。


## 2026-11-05 11:02 Taipei：GPIO部分coverage與UVM資源失敗

開始前可用1268640KiB約1.21GiB，以獨立gpio-20261105-001/180秒、單工split/O0及暫時512MiB RAM/768MiB含swap受控執行。90.95秒退出0無timeout，但不等於全PASS：功能smoke FUNC_PASS checks42，line3/3、toggle67/68、expr2/2；assert/func0/0未量測。

CSV detail明記UVM build make exit2，完整gpio-uvm-build.log證實g++ fatal Killed signal terminated program cc1plus，失敗在Vtop_tb__pch.h.fast.gch。container整體OOMKilled=false不排除子程序資源終止；觀察到507.1MiB/512MiB接近上限，但未取得kernel OOM事件，標partial_functional_coverage_uvm_build_failed並註記資源壓力，不能當GPIO設計錯誤或宣稱全UVM通過。

保存named CSV/execution/profile/log、原始gpio-coverage-evidence及gpio-acceptance.json。原progress及前三CSV SHA皆不變，完成後原容器Memory/MemorySwap恢復，container exited，沒有動其他專案。新增真正功能smoke1項42checks；UVM未動態執行。下一步規劃器明確標detail失敗，PCH資源profile列待評估，不在同資源條件盲目重試。未推送。


## 2026-11-05 11:32 Taipei：checkpoint失敗detail納入重試

plan_vip_coverage_resume.py新增issues、checkpoint_review_required：covered<tests或非空detail保留review；明確UVM build文字另列uvm_build_failed並加入uvm_retry_required，不把CSV存在或covered_tests1當整項PASS。保留原detail與metrics，未將compiler Killed臆測成設計錯誤或kernel OOM證實。

test_vip_checkpoint_plan.py六項6/6通過，新增UVM failure案例仍保留coverage/holes/未量測與pending，且明列retry。真實current清單4checkpoint（UART/SPI/I2C/GPIO）、4舊partial、122無保存coverage、full0；review GPIO，UVM retry舊ACE/ACE Lite/AHB/APB加GPIO共5。原partial timeout及GPIO build_failed各自證據保留，不混為同一失敗原因。

新增模擬0，沒重試GPIO或改寫舊CSV；container exited，diff check通過。下一步PCH記憶體profile用官方/生成make證據確認可行選項，或先續其他VIP；不可在相同512MiB條件盲目重跑。未推送。


## 2026-11-05 12:02 Taipei：PCH生成規則與無PCH dry-run

已從真正5.052 container取出verilated.mk、Vtop_tb.mk/classes.mk及Vtop_tb.cpp，存gpio-pch-analysis。實際規則VM_DEFAULT_RULES非0時FAST/SLOW object各依賴__pch.h.fast/slow.gch；僅改CFG_CXXFLAGS_PCH_I不能移除PCH依賴。VM_PARALLEL_BUILDS=1由output-split產生，與make -j1不同，不應為避PCH改成巨型ALL編譯。

在獨立analysis目錄新增實驗no-pch.mk：VM_DEFAULT_RULES=0並提供%.o:%.cpp普通規則；實際image內128MiB/無網路/唯讀mount執行make -n -f no-pch.mk -f Vtop_tb.mk Vtop_tb.o成功，輸出單一g++ -O0普通compile，沒有.gch或-include fast/slow。這是命令展開實證，非真正C++build或模擬，未改container/verilated.mk全域規則。

限制：實驗規則將所有objects使用OPT_FAST，尚未區分FAST/SLOW/GLOBAL、完整header依賴、全buildlink與單檔RSS；禁PCH可能增加總CPU，不能宣稱降RAM成功。host可用600236KiB約586MiB，原container exited。下一步完善isolated make override的各組flags/依賴，用受限單檔compile量測後才評估GPIO重試，不改預設runner。新增模擬0，未推送。


## 2026-11-05 12:32 Taipei：無PCH各組旗標dry-run驗證

隔離gpio-pch-analysis/no-pch-groups.mk先設VM_DEFAULT_RULES0再include真正Vtop_tb.mk，對VK_OBJS_FAST/SLOW/GLOBAL提供各自OPT_*及OBJCACHE/CXXFLAGS/CPPFLAGS，加入*.d依賴。實際image make -n三個代表target（Vtop_tb.o、ConstPool Slow、verilated.o）各使用不同PROBE macro辨識，3/3命令保留正確group flag且不含.gch/-include PCH；groups-dryrun.log/json保存。不是compile或模擬，未改預設runner。

讀真正Vtop_tb.cpp發現仍#include Vtop_tb__pch.h；關閉預編譯不等於不解析大header，因此不能從dry-run推論RAM下降，可能每個translation unit都付出同樣高成本。下一步若資源允許須先對代表單檔/time最大RSS與原PCH比較、保留timeout/resource_limited，才決定是否重試完整GPIO。

host可用721544KiB約705MiB，本輪僅128MiB短生命無網路容器命令展開，新增RTL/C++build0、模擬0。原container exited，前三PASS與GPIO失敗證據保留，未推送。


## 2026-11-05 13:02 Taipei：無PCH單檔真實編譯量測

從停止容器以tar串流僅取854份生成.h（4333266bytes），保存headers-manifest SHA，不複製大型objects。獨立短生命image容器384MiB RAM/512MiB含swap/1CPU/無網路，以60秒timeout、g++13/gnu++17/O0與coverage/timing定義，真正編譯Vtop_tb.cpp而不產生/使用.gch。47.093秒退出0，Vtop_tb.o存在，RUSAGE_CHILDREN maxRSS386080KiB約377MiB；single-compile.py、compile.log、result.json與object保存gpio-pch-analysis/single-compile-output。

這提供一個代表translation unit可在此限制完成的實證；非完整GPIObuild/link/模擬，也沒有原PCH同條件RSS對照，不能宣稱量化節省或其他巨大UVM方法檔都能過。單檔47秒顯示全量CPU成本可能高，不能直接把無PCH設成預設。顯式編譯命令記錄完整旗標，未聲稱與make所有warning/cache設定完全相同。

host開始前可用564216KiB約551MiB；僅受限單檔工作，原Portfolio容器保持exited，原snapshot/預設runner及已有CSV未改。新增C++單檔成功1、RTL模擬0。下一步按成本評估另一個較大方法TU或PCH對照，再決定是否值得完整GPIO重試；未推送。


## 2026-11-05 13:32 Taipei：PCH對照與OOM事件實證

在相同384MiB RAM/512MiB含swap/1CPU/60秒、同base flags下，真正編譯Vtop_tb__pch.h（-x c++-header）：18.134秒返回1，RUSAGE_CHILDREN maxRSS403244KiB，cgroup memory.events oom+1/oom_kill+1。這次有實際OOM事件佐證，與先前GPIO512MiB僅compiler Killed但未取事件的證據強度不同。

對照上一輪無PCH代表Vtop_tb.cpp47.093秒成功、RSS386080KiB；pch-comparison.json記共同限制。這是header預編譯vs一個TU兩個不同stage，不能聲稱完整build節省固定百分比或每檔都能過。失敗仍留下.gch，已改名.gch.failed-partial保存避免後續誤用，artifact_exists不當成功；原result.json保留真實現象。

host可用1065216KiB約1.02GiB，僅獨立短生命容器，原Portfolio exited、runner/profile/CSV未改。新增真正C++ PCH失敗1（OOM），RTL模擬0。下一步挑較大方法TU在同上限驗證，若也失敗就不投入全量無PCH；同時可續其他VIP，勿為GPIO耗盡回歸進度。

研究去重：英文arXiv與中文OSCC發布搜尋未核實新的24h AI EDA發布；官方news原始頁https://github.com/OSCC-Project/aieda.cc/blob/main/news.html與主repo https://github.com/OSCC-Project/AiEDA為既有來源，不以crawler日期當發布日，未新增能力宣稱。未推送。


## 2026-11-05 14:02 Taipei：最大生成CPP無PCH量測

串流盤點實際GPIO obj共2627個CPP，cpp-size-inventory.json保存檔名/大小；僅取最大Vtop_tb___024root__0__Slow.cpp（198609bytes）及SHA，不複製其他objects。沿相同base flags/O0、384MiB RAM/512MiB含swap/1CPU/60秒單檔編譯，49.010秒返回0、large.o存在；RUSAGE_CHILDREN maxRSS399832KiB，cgroup memory.events max+2466但oom/oom_kill均0。result.json/compile.log及object保存large-compile-output。

成功只代表按檔案bytes最大的TU，不代表語意最複雜、最高RAM的TU；兩個成功樣本仍不足以接受完整GPIO無PCH build。2627個CPP且每個需解析大header，已有47/49秒代表樣本，CPU成本明顯需要評估，不能線性外推固定總時數。結論：無PCH保留實驗選項，暫不更改預設runner或投入全量重編，先續其他VIP一次回歸與Minisim低資源改進，GPIO UVM仍build_failed pending。

host開始前可用599980KiB約586MiB，只有獨立受限短生命容器；原Portfolio仍exited、已有CSV不變。新增C++單檔成功1、RTL模擬0，未推送。


## 2026-11-05 17:02 Taipei：TDM部分coverage／Verilator UVM signal9

從實際來源清單選TDM-UVM-VIP，private19檔與ZIP LF-normalized一致。host可用903948KiB約883MiB，獨立tdm-20261105-001/180秒、512MiB RAM/768MiB含swap、單工split/O0執行；15.24秒完成CSV，無timeout，但不能算整項PASS。

功能smoke FUNC_PASS checks13、t18us；line12/12、toggle543/544、expr8/9，assert/func0/0未量測。UVM build detail為Verilator signal9，與GPIO的g++ PCH cc1plus Killed不同階段；不能因archive較小就推論RAM更少。保存原始tdm-coverage-evidence與tdm-uvm-build.log，保留原始失敗分類，不聲稱RTL設計錯誤／已證實kernel OOM。

原progress及前四CSV SHA不變，完成後容器原Memory/MemorySwap恢復且exited。current清單5checkpoint、4舊partial、121無coverage紀錄、完整0；GPIO/TDM明列review，舊4+兩者UVM retry共6。下一步續其他VIP或依資源profile分層，不在同限制重跑失敗UVM。新增真正functional13checks，UVM未模擬。未推送。


## 2026-11-05 17:32 Taipei：實際SUITES資源分層

取出private tools/uvm_agents規格，僅載入gen.all_specs與run_uvm_tests.SUITES，不呼叫render/main或改VIP。實際SUITES54（註解29為過時描述，不能以註解加AMBA5推成34），全130中76沒有coverage.py的該Verilator UVM分支。對current121項無coverage紀錄：73在功能路徑、48會追加UVM build；此分類不是VIP沒有UVM環境，更不是UVM PASS。

vip-coverage-resource-lanes.json保存每suite test名稱、所有parser/spec SHA及pending兩lane。可先推功能路徑AVSBus/Bluetooth5/CAN等，避免在每項低資源case自動遇到已知大型UVM compiler失敗；上游原suite仍保留不刪、不把延後UVM算通過。規劃器current本身未改歷史結果。

host可用302888KiB約296MiB，原container exited；本輪新增模擬0、程式修改0，完成低資源可行盤點，不啟動編譯。下一步資源允許時依lane選CAN/AVSBus等單項，Minisim VPI最新結果沿用不重跑。未推送。


## 2026-11-05 18:02 Taipei：AVSBus功能lane單項完成

按實際lane選AVSBus-VIP（此coverage.py沒有追加Verilator UVM suite，不代表該VIP沒有UVM環境），private28來源檔與ZIP LF-normalized全相同。host可用909680KiB約888MiB，新avsbus-20261105-001/180秒、512MiB RAM/768MiB含swap、單工split/O0執行9.18秒無timeout。

真正原func_avsbus_tb log FUNC_PASS checks14、t4ns；CSV tests1/covered1，line22/23、toggle18/28、expr14/16，line hole dut_avsbus_regulator_model.sv:43，assert/func0/0未量測，非closure／完整UVM／Minisim PASS。原始dat及真正func_avsbus_tb.log保存在avsbus-coverage-evidence；初次讀固定smoke_func_tb.log不存在已改查實際檔名，不把缺log當設計失敗。

原progress與前五CSV SHA不變，資源設定恢復、container exited。current清單6checkpoint、4舊partial、120無coverage紀錄、full0，GPIO/TDM失敗review不變。下一步續其他功能lane（CAN等）或Minisim需新增的驗證；未推送。


## 2026-11-05 18:32 Taipei：CAN功能lane受限單項完成

CAN-VIP private30來源檔與ZIP LF-normalized全相同，核對實際lane沒有coverage.py追加UVM分支。host可用643184KiB約628MiB，can-20261105-001/180秒採384MiB RAM/512MiB含swap、單工split/O0執行14.66秒，timeout=false。

真正原func_can_tb log FUNC_PASS checks19 xfail0、t113ns；CSV tests1/covered1，line86/96、toggle891/1196、expr42/44，assert/func0/0未量測。10個line holes原文保存，不宣稱完整closure、UVM或Minisim PASS。原dat/log保存can-coverage-evidence，原progress及前六CSV SHA不變。

完成後原容器limits恢復、exited；current清單7checkpoint、4舊partial、119無coverage紀錄、full0，GPIO/TDM仍review。下一步續功能lane其他未測項或資源可行的Minisim實作，不無故重跑正常套件。未推送。


## 2026-11-05 19:02 Taipei：Bluetooth5 coverage建置失敗／未量測

private Bluetooth5-VIP27來源檔與ZIP LF-normalized一致，功能lane無追加UVM。host可用1656960KiB約1.58GiB，新bluetooth5-20261105-001/180秒、384MiB RAM/512MiB含swap、單工profile嘗試，11.86秒返回1、timeout=false。CSV tests1/covered0，全部metric0/0；此為一次真實build attempt，不是動態RTL通過或已量測coverage。

CSV detail含plain make failed及pkgs候選env/env_pkg.sv:13缺include；上游detail截斷400字且未保存完整compile stdout，不能臆測plain根因或稱設計錯誤。保存named CSV/execution/profile/flow及可取出的build/cov目錄、bluetooth5-attempt-classification.json；原progress與前七CSV SHA皆不變，原limits恢復、container exited。

發現規劃器目前要求line正分母會拒絕這類合法失敗紀錄，因此current清單暫保留舊版7個已量測checkpoint；Bluetooth5另外明確記attempted_build_failed_unmeasured，不能當從未嘗試或偷偷刪掉。下一P0修正規劃器區分未量測失敗與非法分母，維持full0、不放寬closure；再核對compile原log保留與有效job數。新增正常RTL模擬0、build失敗1，未推送。


## 2026-11-05 19:32 Taipei：已嘗試但未量測資料保留

plan_vip_coverage_resume.py不再把合法tests1/covered0/全0/0的失敗CSV當非法而拒絕整份清單；分成attempted_coverage_unmeasured，tests0/covered0則no_functional_test_unmeasured，有covered但line0則另列no_dut_line_measurement。全部仍pending/full0，不放寬closure。hit>total/負數仍拒絕，新增covered0卻有非空coverage分母矛盾拒絕。

新增三個subTest覆蓋build_failed未量測、無test、矛盾point，整份七項方法7/7通過，diff check通過。current真實130清單現在8份CSV記錄＝7已量測＋Bluetooth5一項已嘗試未量測，另4舊partial及118項無CSV/partial紀錄；full0。GPIO/TDM仍UVM build issue，Bluetooth5列review而非未嘗試。

本輪新增RTL模擬0，沒有重跑Bluetooth5或改原CSV，原容器仍停止狀態沿用。下一步強化compile完整log與有效job數觀測，再續未測功能lane，避免只用400字detail猜根因。未推送。


## 2026-11-05 20:02 Taipei：工具argv與完整compile log保存

核對rf.run繼承env，舊shell wrapper僅附加-j1於上游-j2之後；Bluetooth5真實摘要仍顯示make-j2，故不能保證末個job旗標生效。新增scripts/verilator_profile.py產生isolated Python wrapper，移除既有-j N/-jN/--build-jobs N/=N後只傳單一-j1；保留原split/O0設定。每次呼叫獨立tool-logs/<time-pid>.json記incoming/effective、.log完整合併stdout/stderr、.exit.json保存真實returncode，避免僅400字CSV摘要。

run_single_vip_coverage採新single-job-normalized-logged-split-O0-v2。真實fakebackend子程序測試2/2通過：四種重複job寫法正規化、失敗7傳遞/完整輸出保存；小量輸出在backend仍sleep時已flush落盤（read1而非等buffer填滿）。受影響單項execution/CLI/Pool五方法5/5通過，diff check通過。這不是用真Verilator驗證make-j1，本輪新增RTL模擬0；下一實際attempt仍需核對effective argv與真make command。

舊Bluetooth5摘要不能追溯補完整log，保留根因未知；未修改原CSV或重跑失敗。新profile僅後續啟動生效，原container exited。下一步受控新功能VIP或有理由的Bluetooth5診斷attempt，保存完整compile evidence；未推送。


## 2026-11-05 20:32 Taipei：Bluetooth5新profile診斷重試成功

新normalized/logged profile已變更，故做一次有依據重試，原attempt001/全部歷史CSV及progress SHA保留。host可用715448KiB約699MiB，相同384MiB RAM/512MiB含swap、180秒、bluetooth5-20261105-002執行14.34秒，timeout=false、返回0。真正原test FUNC_PASS checks28 xfail0、t7ns；line23/24、toggle131/378、expr10/11、assert/func0/0未量測，hole dut_bluetooth5_dut.sv:125。

tool-logs保存完整實際incoming/effective argv：既有-j2移除後唯一-j1；真Verilator build/run成功，job-diagnostic.json保存對照。成功log只列make entering/leaving，沒有觀察OS層所有make子程序argv，不誇大證據。這支持新profile在此相同限制可完成，但不同時間系統狀態未鎖定，不聲稱-j2為舊失敗唯一根因。完整log沒有error。

原limits恢復、container exited。current顯式選擇Bluetooth5 attempt002（來源同版），001在prior_attempts_retained記SHA與失敗；同名衝突規則未移除，未靜默覆蓋。現8個unique已量測、0目前未量測、118無紀錄、4舊partial、full0；實際CSV attempt共9，舊失敗仍留。下一步沿新profile推其他未測功能lane，GPIO/TDM UVM仍review；未推送。


## 2026-11-05 21:32 Taipei：CCIX功能lane單項完成

CCIX-VIP private28來源檔與ZIP LF-normalized一致，核對實際lane。host可用547356KiB約535MiB，新ccix-20261105-001/180秒、384MiB RAM/512MiB含swap、normalized/logged單工profile，3.59秒完成無timeout。原func_ccix_tb FUNC_PASS checks13、t250ps；line11/12、toggle207/282、expr6/7，line hole dut_ccix_agent_model.sv:46，assert/func0/0未量測，不宣稱closure／完整UVM或Minisim PASS。

保存named CSV/完整tool-logs與ccix-coverage-evidence原dat/log，原progress與所有舊CSV（含Bluetooth5失敗001）SHA不變，原limits已恢復、container exited。current沿顯式checkpoint_sources加本次而非glob自動選latest，保留prior_attempts_retained。現9個unique已量測、4舊partial、117無紀錄、full0；GPIO/TDM仍review。新增真實13checks，未推送。下一步續其他功能lane或實用Minisim整合。


## 2026-11-06：checkpoint完整性稽核

低記憶體（可用約473MiB）本輪未啟動編譯。current九份選擇CSV加保留Bluetooth失敗001，共10/10 SHA吻合；證據Helios build/portfolio-extended-20261101/checkpoint-integrity-20261106.json。9已量測、4舊partial、117無紀錄、full0維持，新增RTL測試0。下一項CPRI僅於資源可行時受控單項：來源一致、真FUNC checks、完整log/CSV與歷史SHA不變為驗收，未宣稱Minisim PASS。每日研究見Helios docs/research/2026-11-06.md。


## 2026-11-06 11:43 Taipei：CPRI功能單項完成

host可用635328KiB約620MiB、container exited後，CPRI private28檔與上游ZIP逐檔LF-normalized一致，確認不在上游Verilator UVM suite。cpri-20261106-001單工normalized/logged profile、384MiB RAM/512MiB含swap、180秒限制，10.59秒返回0且無timeout。原func_cpri_tb.log真實FUNC_PASS checks=17；CSV tests1/covered1、line17/18、toggle140/152、expr9/9，line hole dut_cpri_slave_dut.sv:44；assert/func0/0未量測，非完整closure、UVM或Minisim PASS。

原dat/log及來源核對、acceptance保存在Helios build/portfolio-extended-20261101/cpri-preflight-20261106，named結果含完整tool logs。所有既有coverage_report.csv及progress SHA不變，原memory/swap已恢復、容器exited。current顯式加入本次CSV並保留Bluetooth001，10 unique已量測、4舊partial、116無紀錄、full0；新增真實17checks。今日研究沿用已核實來源，不重複列新發布。下一步CSE功能單項（資源可行時），GPIO/TDM UVM失敗保留，未推送。


## 2026-11-06 12:13 Taipei：CSE來源與驗收準備

host可用395504KiB約386MiB，Portfolio仍exited，其他buildkit未動；本輪未啟動編譯。CSE private22來源與上游ZIP逐檔LF-normalized吻合，確認功能lane。靜態檢閱原func_cse_tb：34處chk呼叫（不是動態checks），包含命令延遲/IRQ、register/memory隔離、config/sticky error、busy operand、sideband、Z bus release及回應中reset。來源SHA/source-check及acceptance-plan保存Helios build/portfolio-extended-20261101/cse-preflight-20261106。

未建立single request或attempt，CSE維持未嘗試；新增RTL測試0。current仍10項已量測、4舊partial、116無紀錄、full0。下一步資源可行時唯一attempt/180秒/既有單工profile，驗收真FUNC_PASS正checks、無FUNC_FAIL、完整CSV/raw/tool logs與歷史SHA不變／資源恢復。SVA/func空分母不可當closure。今日研究沿用已核實來源，本輪無新發布宣稱。分支仍codex/且diff check通過，未推送。


## 2026-11-06 13:13 Taipei：CSE真實功能回歸完成

修正資源判斷方式：Windows free338848KiB約331MiB，但WSL free顯示available6420MiB、swap幾乎未用，Docker MemTotal8110399488。只看Windows free不足以判定受限容器無法執行；後續同時核對host/WSL及其他jobs，不把container上限當最低需求。

已核對CSE22份來源SHA仍吻合；cse-20261106-001以既有單工normalized/logged profile、384MiB RAM/512MiB含swap、180秒執行29.93秒，返回0且無timeout。原func_cse_tb.log真實FUNC_PASS checks34、無FUNC_FAIL；CSV tests1/covered1、line43/44、toggle130/628、expr7/7，hole dut_mipi_dut.sv:134。SVA/func0/0未量測，不宣稱closure、完整UVM或Minisim PASS。

raw dat/log、來源核對與acceptance在Helios build/portfolio-extended-20261101/cse-preflight-20261106，named CSV/tool logs保留。所有歷史coverage CSV及progress SHA不變，原memory/swap恢復、container exited。current明確加入CSE並保留舊失敗，11已量測、4舊partial、115無紀錄、full0。新增真實34checks；下一步CSI_2單項（先查host與WSL），Minisim property pending補測待辦保留。今日研究沿用既有核實來源，未推送。


## 2026-11-06 13:43 Taipei：CSI_2功能單項完成

Windows free545424KiB約533MiB，WSL available6417MiB且swap幾乎未用；其他buildkit未動。CSI_2 private24檔與ZIP逐檔LF-normalized一致，確認上游功能lane。csi2-20261106-001既有normalized/logged單工profile、384MiB RAM/512MiB含swap、180秒限制，11.52秒完成返回0且無timeout；原func log真實FUNC_PASS checks29、無FUNC_FAIL。CSV tests1/covered1、line13/13、toggle187/362、expr10/10；assert/func0/0未量測，line全命中不等於完整closure／UVM或Minisim PASS。

原dat/log、source-check及acceptance在Helios build/portfolio-extended-20261101/csi2-preflight-20261106，named CSV/tool logs保留。歷史所有coverage_report.csv及progress SHA不變，原memory/swap恢復、container exited。current顯式加入CSI_2、舊失敗保留；12項已量測、4partial、114無紀錄、full0。新增真實29checks，diff check通過。下一步CXL單項先核對lane/來源與資源，Minisim property pending驗收待辦保留；今日研究不重複已有來源為新發布，未推送。


## 2026-11-06 14:13 Taipei：CXL部分功能coverage／Icarus限定保留

Windows free496848KiB約485MiB、WSL available6428MiB，其他buildkit未動。CXL private32檔與上游ZIP LF-normalized一致，功能lane；cxl-20261106-001單工384MiB RAM/512MiB含swap、180秒，40.65秒返回0無timeout。原func_cxl_mem_tb checks29及smoke_func_tb checks13真實FUNC_PASS；但三個test僅兩個covered，func_cxl_bundle_tb.sv上游明確SKIP pinned to Icarus，不把return0當整項PASS或任意waiver。

CSV line73/75、toggle3646/6826、expr46/51；holes dut_cxl_mem_model.sv:102,134，assert/func0/0未量測。完整資料cxl-preflight-20261106/coverage-evidence與named CSV/tool logs，acceptance明列partial。舊CSV/progress SHA不變、資源恢復、container exited。current13已量測、4舊partial、113無紀錄、full0；CXL列functional_tests_not_all_covered及detail review。新增兩個真實功能tests共42checks，不代表完整UVM或Minisim PASS。

下一P0檢閱上游pinned-Icarus入口及其既有歷史結果，設計同revision獨立Icarus補測並與Verilator coverage分開；不得將Icarus PASS補成Verilator coverage bins。再續CXS。Minisim pending評測待辦保留；今日研究沿用已核實來源，未推送。


## 2026-11-06 14:43 Taipei：CXL Icarus歷史證據對應

上游cxl_bundle/tb/func_cxl_bundle_tb.sv明列FUNC_SIM: iverilog。找到既有vip-basic/CXL-VIP/functional.log明列bundle [iverilog checks=34]，functional-evidence/func_cxl_bundle_tb.log.gz原始FUNC_PASS checks34、無FUNC_FAIL，結束42531000ps。來源revision由launch-manifest記ba3cf117a2d18713df5c6562d3428afa896a8a94；本次32來源與同ZIP已比對，但舊執行binary沒有完整content provenance，不冒稱新建置驗證。

保存icarus-historical-crossreference.json，含原gzip/log/測試來源/launch manifest SHA，current另加independent_functional_evidence連結；原CSV不變，CXL Verilator covered2/3及review維持，不能將Icarus PASS灌入Verilator line/toggle/expr bins。這說明basic phase三項功能已有歷史PASS（13+34+29），不是本輪新測試、非完整UVM／Minisim或RTL closure。

新增模擬0，避免無理由重跑既有PASS；current13已量測、4partial、113無紀錄、full0不變。下一步CXS單項，Minisim pending評測待辦保留；今日研究無新發布宣稱，未推送。


## 2026-11-06 15:13 Taipei：CXS功能單項完成

Windows free1342364KiB約1.28GiB，WSL available6470MiB，其他buildkit未動。CXS private28檔與上游ZIP LF-normalized一致；cxs-20261106-001既有normalized/logged單工profile、384MiB RAM/512MiB含swap、180秒限制，26.35秒返回0且無timeout。原func_cxs_tb.log FUNC_PASS checks11、無FUNC_FAIL；CSV tests1/covered1、line13/13、toggle164/174、expr18/18，assert/func0/0未量測，非完整closure、UVM或Minisim PASS。

保存cxs-preflight-20261106來源/raw evidence/acceptance與named CSV/tool logs；歷史CSV/progress SHA不變，資源恢復、container exited。current顯式加入CXS並保留prior_attempts_retained及CXL independent_functional_evidence；14已量測、4partial、112無紀錄、full0。新增真實11checks。下一步C_PHY單項，Minisim property pending驗收待辦保留；今日研究沿用已核實來源，未推送。


## 2026-11-06 15:43 Taipei：C_PHY功能單項完成

Windows free1371060KiB約1.31GiB、WSL available6437MiB；其他buildkit未動。C_PHY private23來源與ZIP逐檔LF-normalized一致，功能lane；cphy-20261106-001既有normalized/logged單工profile、384MiB RAM/512MiB含swap、180秒限制，37.74秒返回0無timeout。原func_mipi_tb.log真實FUNC_PASS checks29、無FUNC_FAIL；CSV tests1/covered1、line13/13、toggle187/362、expr10/10，assert/func0/0未量測，不代表完整closure、完整UVM或Minisim PASS。

來源/raw dat/log/acceptance存Helios build/portfolio-extended-20261101/cphy-preflight-20261106，named CSV/tool logs保留。歷史CSV/progress SHA不變，資源恢復、container exited；current顯式加入本次、保留舊失敗及CXL independent evidence。15已量測、4partial、111無紀錄、full0；新增真實29checks。下一步DBI單項，Minisim property pending驗收待辦仍保留；今日研究不重複既有來源當新發布，未推送。


## 2026-11-06 16:13 Taipei：DBI功能單項完成

Windows free1056332KiB約1.01GiB、WSL available6465MiB，其他buildkit未動。DBI private28來源與ZIP逐檔LF-normalized一致，功能lane；dbi-20261106-001既有normalized/logged單工profile、384MiB RAM/512MiB含swap、180秒限制，23.57秒返回0且無timeout。原func_dbi_tb.log真實FUNC_PASS checks15、無FUNC_FAIL；CSV tests1/covered1、line28/28、toggle62/68、expr10/10，assert/func0/0未量測，非完整closure、UVM或Minisim PASS。

保存dbi-preflight-20261106來源/raw evidence/acceptance及named CSV/tool logs；歷史CSV/progress SHA不變，資源恢復、container exited。current顯式加入DBI並保留舊失敗及CXL獨立Icarus連結；16已量測、4partial、110無紀錄、full0，新增真實15checks。下一步DDR單項，Minisim property pending待辦保留；今日研究不重複既有來源當新發布，未推送。


## 2026-11-06 16:43 Taipei：DDR功能單項完成

Windows free1649308KiB約1.57GiB、WSL available6452MiB；其他buildkit未動。初始suffix來源比對因巢狀README同尾名歧義而停止，未更改request/資源；改用ZIP根DDR-VIP/精確路徑後23檔LF-normalized全吻合，沒有內容差異或RTL修改。

ddr-20261106-001既有normalized/logged單工profile、384MiB RAM/512MiB含swap、180秒限制，38.59秒返回0無timeout；原func_mem_tb.log真實FUNC_PASS checks20、無FUNC_FAIL。CSV tests1/covered1、line23/23、toggle216/1274、expr22/24，assert/func0/0未量測，非完整closure、UVM或Minisim PASS。

保存ddr-preflight-20261106 source-check/raw evidence/acceptance與named CSV/tool logs；歷史CSV/progress SHA不變，資源恢復、container exited。current顯式加入DDR且保留舊失敗/CXL獨立證據，17已量測、4partial、109無紀錄、full0。新增真實20checks。下一步依current與lane選下一未測項，來源比對沿精確ZIP根路徑；Minisim pending驗收待辦保留，今日研究不重複舊來源當新發布。未推送。


## 2026-11-06 17:13 Taipei：DFI功能單項完成

WSL available6461MiB、swap幾乎未用，Portfolio原exited。current/lane選出DFI；private28來源與ZIP精確根路徑LF-normalized吻合。dfi-20261106-001既有單工normalized/logged profile、384MiB RAM/512MiB含swap、180秒限制，57.67秒返回0無timeout。原func_dfi_tb.log真實FUNC_PASS checks29、無FUNC_FAIL；CSV tests1/covered1、line26/27、toggle318/684、expr9/11，hole dut_dfi_dram_model.sv:105；assert/func0/0未量測，不等於完整closure、UVM或Minisim PASS。

保存dfi-preflight-20261106來源/raw evidence/acceptance及named CSV/tool logs，歷史CSV/progress SHA不變，資源恢復、container exited。current顯式加入DFI，保留舊失敗及CXL獨立證據；18已量測、4partial、108無紀錄、full0。新增真實29checks，下一步DPI單項；Minisim property pending待辦保留，今日研究無新增發布宣稱。未推送。


## 2026-11-06 17:44 Taipei：DPI功能單項完成

WSL available6396MiB、Portfolio原exited；DPI private28來源與ZIP精確路徑LF-normalized吻合。dpi-20261106-001既有單工normalized/logged profile、384MiB RAM/512MiB含swap、180秒限制，25.2秒返回0無timeout。原func_dpi_tb.log FUNC_PASS checks10 dut_errors=3；檢閱來源第85-86行，dut_errors=3是固定字串，對應short frame/DE-in-VSYNC/long frame預期錯誤，不是實際計數器實測值，不能聲稱動態觀測到恰好三次錯誤。10個chk通過仍為上游測試結果。

CSV tests1/covered1、line20/20、toggle44/66、expr15/15，assert/func0/0未量測，非完整closure、UVM或Minisim PASS。保存dpi-preflight-20261106來源/raw evidence/acceptance及named CSV/tool logs；歷史CSV/progress SHA不變，資源恢復、container exited。current保留舊失敗/CXL獨立證據，19已量測、4partial、107無紀錄、full0。下一步DSI；後續若深化DPI checker須加入獨立實際error report計數驗收，不以固定字串作證。今日研究無新發布宣稱，未推送。


## 2026-11-06 18:14 Taipei：DSI功能單項完成

WSL available6451MiB，Portfolio原exited、其他buildkit未動。DSI private23來源與ZIP精確路徑LF-normalized吻合；dsi-20261106-001既有單工normalized/logged profile、384MiB RAM/512MiB含swap、180秒限制，20.74秒返回0無timeout。原func_mipi_tb.log真實FUNC_PASS checks29、無FUNC_FAIL；CSV tests1/covered1、line13/13、toggle187/362、expr10/10，assert/func0/0未量測，非完整closure、UVM或Minisim PASS。

保存dsi-preflight-20261106來源/raw evidence/acceptance及named CSV/tool logs；歷史CSV/progress SHA不變、資源恢復、container exited。current顯式加入DSI、保留失敗及CXL獨立證據，20已量測、4partial、106無紀錄、full0；新增真實29checks。下一步D_PHY單項，Minisim property pending待辦保留，今日研究不重複既有來源當新發布，未推送。


## 2026-11-06 18:44 Taipei：D_PHY功能單項完成

WSL available6459MiB，systemd user-session警告後free查詢正常，未重啟WSL；Portfolio原exited。D_PHY private23來源與ZIP精確路徑LF-normalized吻合；dphy-20261106-001既有單工normalized/logged profile、384MiB RAM/512MiB含swap、180秒限制，26.12秒返回0無timeout。原func_mipi_tb.log真實FUNC_PASS checks29、無FUNC_FAIL；CSV tests1/covered1、line13/13、toggle187/362、expr10/10，assert/func0/0未量測，非完整closure、UVM或Minisim PASS。

保存dphy-preflight-20261106來源/raw evidence/acceptance及named CSV/tool logs；歷史CSV/progress SHA不變、資源恢復、container exited。current顯式加入並保留歷史失敗/CXL獨立證據，21已量測、4partial、105無紀錄、full0；新增真實29checks。下一步DigRF單項，Minisim property pending待辦保留，今日研究無新增發布宣稱，未推送。


## 2026-11-06 19:14 Taipei：依使用者要求轉coverage收斂／DBI random準備

優先改為補holes而非繼續逐項baseline。DBI DUT baseline62/68 directional toggle，六個缺口為slv_d_o[1]及cur_cmd[6]/[7]各rise/fall；已從raw dat精確取出，與testbench bins分開。private擴充保留原15 checks，固定xorshift seeds1/2/3/4各128筆高位命令與隨機參數，逐筆readback及cur_cmd檢查，預期每run271 checks；這些目前是待執行，不是PASS。

證據dbi-random-20261106：初建置缺common_pkg明確拒絕，原log保留；補上基線同package與coverage旗標後，隔離384MiB/512MiB、1CPU、無網路容器建置150秒timeout，新增模擬0／coverage增益0。容器--rm已移除，原Portfolio未啟動、其他容器未動。run.py下一次已改stdout直接落盤，避免timeout前capture buffer無法保存完整compile診斷；尚未重跑。

收斂狀態：baseline62/68不變、六holes仍待補，不能說已收斂。下一步用保留obj/log診斷建置成本，固定source/denominator後跑seeds並逐輪合併相同DUT bin keys、記新增hits與plateau；不得混testbench bins或將0/0當完成。仍21項已量測、105無紀錄；停用「完整驗收0」用語，逐項列功能/coverage/未完成原因。Minisim與外部模擬分開，未推送。


## 2026-11-06 19:44 Taipei：DBI random實跑與平台期

上次150秒timeout留下不完整PCH，本輪同obj增量續建被編譯器明確invalid PCH拒絕，log另存。改容器本地/tmp/dbiobj乾淨建置，同DUT/package/bench與coverage flags、單工384MiB/512MiB、無網路成功；原損壞obj保留。不是直接放寬資源或忽略失敗。WSL available6402MiB，原Portfolio/其他容器未動。

seeds1/2/3/4各128筆高位命令＋隨機參數寫讀，每run271checks（原15＋新256）皆FUNC_PASS，合計512隨機交易、1084checks。DUT raw keys以來源basename正規化後逐key集合與baseline完全一致，不合併testbench；seed1新增4toggle，62/68→66/68，seed2/3/4新增0，明確平台期。剩cur_cmd[6]:1->0、cur_cmd[7]:1->0；高命令mask C0使兩bit隨機段一直為1，下一輪加入低命令或reset後檢查，並保留同分母對照。

保存dbi-random-20261106/convergence.json逐seed累積、raw SHA、remaining、run-manifest來源/binary SHA及各seed raw/log。line原28/28與expr10/10未宣稱新提升；raw line point10不等於展開line28，不混分母。SVA/func未量測，仍未完整closure，非Minisim PASS。baseline CSV維持歷史62/68，不覆寫；status另列random_pass_plateau。隔離容器已自動清理，新增真正random成果如上，未推送。


## 2026-11-06 20:14 Taipei：DBI random＋定向補洞收斂

DUT/package內容與上一輪完全不變，保留seeds1/2/3/4各128筆隨機段，段末加入cmd36/prm02/readback，獨立核對cur_cmd清高位與讀回值；每run273checks全數FUNC_PASS（共1092checks、512隨機＋4定向交易），非單純toggle刺激不驗值。隔離1CPU/384MiB RAM/512MiB含swap、無網路，本地tmp乾淨建置成功，WSL available6408MiB。

逐key確認四run DUT分母與baseline一致，僅來源路徑basename正規化、排除testbench bins。baseline62/68→上一輪純隨機66/68平台→本輪seed1補齊68/68，seed2-4仍68/68；cur_cmd[6]/[7]下降沿皆有實際hit，remaining空。原line28/28、expr10/10獨立分母維持，不把raw line point10當28。僅此DBI已登記DUT line/toggle/expr scope完成，SVA/covergroup未量測、非完整RTL closure／Minisim PASS。

保存dbi-random-close-20261106/convergence.json逐seed與raw SHA、run-manifest來源/binary SHA、四份raw/log；歷史baseline CSV與平台期紀錄不覆寫。隔離容器自動移除、原Portfolio未啟動。下一步依效益選下一個可達coverage holes，沿用固定seed→plateau→定向補洞與同分母驗收，不再只增加baseline項目。未推送。


## 2026-11-06 20:44 Taipei：SPI缺口定位與random規劃

DBI同分母68/68結果保留，本輪轉下一項SPI。原baseline line22/25、toggle46/50、expr10/13；逐raw DUT points找到reset下降沿、mosi_oe下降沿、rx_shift[7]雙向、reset-low frame分支與CPHA相關expr分支。原smoke僅mode0且MOSI一直driven，因此盲目多seed不會命中CPHA分支。

spi-closure-20261106保存原flattened DUT及plan.json（完整holes、baseline SHA、固定seeds1..4）。下輪driver需依CPOL/CPHA模式正確設定MOSI與MISO取樣，獨立previous-frame queue oracle，隨機byte/長短/空幀，並驗reset及MOSI enable。rx_shift[7]於第8bit被賦值後同程序立即清0，可能是瞬態toggle取樣缺口，尚未動態驗證或豁免；須用byte scoreboard及實際coverage instrumentation分開判斷。

本輪新增模擬0，僅定位/設計，不宣稱SPI增益。下一步實作有oracle的固定seed模式回歸、同DUT keys合併及plateau紀錄；DBI/SPI均非Minisim PASS，SVA/covergroup空分母仍未量測。未推送。


## 2026-11-06 21:14 Taipei：SPI四模式random實跑與平台期

沿用原DUT、不改RTL，原mode0十checks保留；增加CPOL/CPHA四模式、每模式16幀隨機byte，獨立previous-frame byte oracle逐幀比對MISO與CS後Z釋放；reset-low CS補測。seeds1/2/3/4每run139checks全FUNC_PASS，共256隨機幀、556checks。WSL available6434MiB，隔離1CPU/384MiB RAM/512MiB含swap、無網路、tmp乾淨建置成功，原Portfolio未啟動。

逐key確認DUT raw bins與baseline集合完全一致，source basename正規化，排除testbench。seed1新增6 raw points，後三seed增益0：toggle46/50→48/50、expr10/13→12/13；branch raw18/18，raw line6/7（不是展開line分母25，未聲稱line25/25）。剩rx_shift[7]rise/fall、line block及(vif_mosi_oe==0)=>1 expr。功能四模式比對通過不代表其expr bin已命中；尤其同activation瞬態／coverage取樣疑慮尚未證實，不豁免、不稱收斂。

保存spi-closure-20261106/convergence.json逐seed/remaining/raw SHA、run-manifest來源/binary SHA與原log；baseline CSV維持歷史值。下一步核對generated C++ coverage點觸發時機，將模式setup保持時間與rx_shift瞬態分開診斷，以獨立byte scoreboard保護語意；需要實測才能分類工具限制。隔離容器自動移除，未推送。


## 2026-11-06 21:44 Taipei：SPI expr補洞與generated instrumentation診斷

相同RTL emit C++證實：expr(vif_mosi_oe==0)計數在幀結束後root__1.cpp914-918，不在CPHA解碼點；原隨機driver解碼後恢復oe1，故雖模式比對PASS仍未命中此expr。本輪僅在幀結束前設oe0，同4seeds、每64幀及139checks再次全PASS，共256幀/556checks；expr12/13→13/13，branch raw18/18，toggle仍48/50、raw line6/7。

剩餘分類有source/generated證據：rx_shift每次從0逐bit移入，第8bit賦值後立即push並清0（843-851），bit7為同activation瞬態，不能靠更多正常frame在yield點觀測；line43 bin對應forever迴圈後co_return942，為生成的不可達結束點。這是具體取樣/結構分類，原分母不刪、未豁免，仍回報toggle48/50與line缺口，不冒稱100%完整closure。

spi-closure-end-20261106保存四run/raw/log、convergence與instrumentation-evidence generated檔SHA及位置。DUT不改、baseline與前輪平台期保留；隔離容器自動移除，原Portfolio/其他容器未動。下一步把工具取樣限制與可達functional驗收分開建立審查項，再選下一個可達holes；不能持續無效盲跑seed。非Minisim PASS，SVA/covergroup仍未量測，未推送。


## 2026-11-06 22:14 Taipei：收斂證據接回清單／共用稽核

新增Helios scripts/coverage_convergence.py共用exact raw-point audit：必須明列DUT basename，排除testbench；每run key集合與baseline相同才合併，空scope／無run拒絕，逐輪增益/remaining/raw SHA保存。工具明確source_equivalence_verified=false（只比bins，不代替來源SHA核對），full_rtl_closure=false；raw line point與展開line分母不同。五項單元測試5/5 PASS：gain/plateau/TB排除、changed denominator拒絕、empty DUT拒絕、empty runs拒絕、remaining不豁免。

兩組真實既有raw files以共用工具重算：DBI68/68、SPI48/50均與原結論一致，shared-audit.json與SHA各保存，新增模擬0。current另加coverage_convergence_evidence連結，DBI標measured_scope_closed、SPI gaps classified pending review，所有historical checkpoint CSV與prior evidence維持，不將全域固定0當驗收數。下一補測沿用此共用audit，避免反覆複製merge runner；未推送。


## 2026-11-06 22:44 Taipei：CXS固定seed隨機收斂

CXS baseline164/174缺口全為控制輸入cntl[3..7]雙向，原packet只送小i。DUT不改，保留原11checks；新增seeds1/2/3/4各64包、長度1..4 flits、隨機data/cntl，逐包沿原reference CRC8函式及packet count比對。四run每75checks全FUNC_PASS，合計256隨機包/300checks；WSL available6422MiB、隔離單工384MiB/512MiB、無網路、tmp乾淨建置。CRC oracle沿用原bench，不宣稱新獨立數學實作。

共用coverage_convergence audit確認相同DUT raw keys，toggle164/174→174/174、remaining空；各run/raw/log/manifest與shared-audit SHA存cxs-random-20261106，current另連measured_scope_closed。這十bins是輸入控制欄位切換；DUT是否使用cntl需看模型語意，不能以輸入toggle當協定完整功能驗證。line/expr既有scope通過但SVA/functional covergroup未量測，非完整RTL closure／Minisim PASS。歷史CSV不覆寫，隔離容器自動移除、其他容器未動。下一步選下一組可達holes，未推送。


## 2026-11-06 23:14 Taipei：同分母合併的來源防混合

coverage_convergence新增拒絕同raw內同basename不同完整路徑、缺少任一指定DUT；可提供baseline加每run的exact DUT SHA256 records，缺筆/格式錯/內容變更皆拒絕。相同紀錄只標recorded_source_sha256_agreement，source_equivalence_verified仍false，避免把宣告一致冒充hermetic binary provenance。原本只比bins的模式仍明列限制。

新增五方法後整份10/10單元PASS，含真負例basename collision/missing source/changed digest/malformed records；使用三組真實既有raw重新稽核DBI68/68、SPI48/50、CXS174/174均一致。報告convergence-source-guards-20261106.json，新增模擬0，不提高coverage數字、不覆寫歷史audit。下一個random工作沿固定來源雜湊、相同bins與功能oracle，SPI未解取樣點不盲跑或自動豁免。未推送。


## 2026-11-06 23:44 Taipei：GPIO reset＋固定seed隨機收斂

GPIO baseline67/68唯一缺口rst_n下降沿，原bench僅啟動解除reset。DUT不改，保留原42checks並將原20隨機cycles改固定xorshift seed，加入運行中reset/輸出與direction清0檢查、再128隨機cycles驗值。seeds1/2/3/4每300checks全FUNC_PASS，共592隨機cycles、1200checks；WSL available6375MiB，隔離1CPU/384MiB RAM/512MiB含swap、無網路、tmp乾淨建置。

共用audit確認相同DUT raw keys，toggle67/68→68/68，line/expr原scope維持；shared-audit/raw/log/manifest存gpio-random-20261106。current另連functional_measured_scope_closed_uvm_build_failed：只此功能量測範圍閉合，舊UVM PCH compiler failure與review完整保留，不能說整項UVM PASS或Minisim PASS。SVA/covergroup未量測，baseline CSV不覆寫。隔離容器自動清理、其他容器未動；下一步繼續可達coverage holes，未推送。


## 2026-11-07 00:44 Taipei：TDM隨機幀與reset補洞

TDM baseline543/544唯一toggle缺口rst_n下降沿；DUT不改，保留原13checks，加中途reset報告清0檢查及每seed32幀128bit固定xorshift隨機資料，逐槽report/count/error比對。seeds1/2/3/4每46checks全FUNC_PASS，共128隨機幀、184checks；WSL available6451MiB，隔離1CPU/384MiB RAM/512MiB含swap、無網路、tmp乾淨建置。

共用audit確認相同DUT keys，toggle543/544→544/544，expr仍8/9，剩(fsync==1)=>1；不宣稱所有scope閉合。該expr位於完成bit條件，但fsync1會優先進入新幀分支，需再核對generated instrumentation/可達性，暫保留未解、不豁免。tdm-random-20261107存raw/log/manifest/shared-audit，current標toggle_closed_expr_gap_uvm_build_failed；原UVM Verilator signal9失敗/review保留、非完整UVM或Minisim PASS。歷史CSV不覆寫，隔離容器移除，其他容器未動，未推送。


## 2026-11-07 01:14 Taipei：TDM未命中expr守衛窮舉

將原RTL exact guard片段與SHA綁定，對resetn/active/fsync已知0/1、nbit unsigned8bit全部2048組做獨立Python布林窮舉。只有一組能進completion expr（reset1/active1/nbit127/fsync0），fsync1 witness為0；因fsync1且nbit127先進新幀分支，無法落入後面的完成分支。移除高優先分支的guard mutant產生fsync1反例一組，確認檢查有敏感度。

fsync-guard-analysis.json記來源SHA、假設、結果及counterexample，fsync-guard-table.json保存全部表。這是有限二態守衛枚舉，不是RTL模擬、parser、時序formal或四態proof；coverage原expr8/9維持，分類candidate structurally blocked、沒有自動waiver。toggle544/544與UVM build failure仍各自保留。本輪新增模擬0，不再盲目多跑seed。下一步選其他可達holes或補Minisim property末拍驗收；未推送。


## 2026-11-07 01:44 Taipei：UART固定seed echo/reset補洞

UART toggle9/10唯一缺口reset下降沿。DUT不改，private原bench的61 checks（含14-frame burst及bad-stop/parity/glitch）保留，增加idle中reset檢查與seeds1/2/3/4各16 random bytes，沿原獨立echo decoder逐資料/parity slot/stop比對。四run每110checks全FUNC_PASS，共64新增隨機frames、440checks；與先前31-check歷史basic/profile不同，不覆寫舊結果。WSL available6446MiB，隔離單工384MiB/512MiB、無網路、tmp乾淨建置。

共用audit確認相同DUT raw keys，toggle9/10→10/10，expr4/4原scope維持；兩個line raw holes仍保留待檢視長駐程序結束點，不直接稱line100%。保存uart-random-20261107 raw/log/manifest/shared-audit，current另連toggle_closed_line_gaps_pending_review。SVA/covergroup未量測，非完整closure或Minisim PASS；原lowmem UVM88交易獨立證據不混本次功能checks。baseline CSV不覆寫，隔離容器移除、其他容器未動，未推送。


## 2026-11-07 02:14 Taipei：UART line endpoint分類

相同UART RTL emit C++（不建置／不跑sim）對應line21/49兩個未命中bin，registry與實際counter位置皆保存line-endpoint-analysis.json；兩者increment位於RX/TX unconditional forever迴圈之後，緊接co_return。原RTL確有兩個長駐無break程序，因此分類為生成的程序結束點，不應盲目增加seed追求這兩個line bins。

這是source/generated靜態證據，非formal proof；原分母與raw14/16 line、toggle10/10不改，不自動waiver、不冒稱line100%。保存DUT SHA、原檔位置與generated snippets；新增模擬0，原功能440checks與獨立UVM88交易保持分開。下一步其他可達holes或Minisim property drain驗收，隔離emit容器已清理、其他容器未動，未推送。


## 2026-11-07 02:44 Taipei：CPRI固定seed資料loopback補洞

CPRI12個toggle缺口集中tx/rx特定位元。核對原tool argv選取與raw baseline相同packaged DUT/cpri_defs/common_pkg成功輸入組合，不用首次無package候選；原DUT不改。保留17checks，新增解除reset後4comma重同步檢查及seeds1/2/3/4各64隨機32bit資料與K flag、逐字rx比對。四run每82checks全FUNC_PASS，共256隨機words/328checks；WSL available6456MiB，隔離單工384MiB/512MiB無網路、tmp乾淨建置。

共用audit確認相同DUT keys，toggle140/152→152/152；其他raw scope及remaining見shared-audit，未將toggle閉合外推整項驗收。cpri-random-20261107保留成功來源組合、raw/log/manifest/audit；current另連toggle閉合與剩餘scope審查，歷史CSV不覆寫。SVA/covergroup未量測，非完整RTL closure／Minisim PASS；隔離容器移除、其他容器未動。未推送。


## 2026-11-07 03:14 Taipei：CPRI飽和branch補洞

剩branch是comma_cnt<15的else，先前未連續送到飽和。DUT/package不改，在原seed1隨機64words段後送20comma，再送非comma active資料，檢查comma_cnt==15、synced、LINK_ACTIVE及資料loopback。單run83checks全FUNC_PASS，不無故重跑其他三seed。WSL available6572MiB，隔離單工384MiB/512MiB無網路、tmp乾淨建置。

共用audit以baseline＋先前四seed＋本次逐key合併，branch15/16→16/16、toggle152/152、expr9/9，全部已登記raw DUT points remaining空；raw line2/2與展開line18分母不同，未冒稱full language。cpri-saturation-20261107保留raw/log/manifest/audit，current標measured_raw_scope_closed；SVA/covergroup未量測、非完整RTL closure或Minisim PASS。歷史CSV與舊run保留，隔離容器移除、其他容器未動，未推送。


## 2026-11-07 03:44 Taipei：CPRI random scoreboard真DUT mutant拒絕

獨立private mutant僅將rx_data賦值遮掉bit16，clean DUT/package/test保持獨立保存；seed1真實82checks出現36個random loopback資料mismatch，FUNC_FAIL被預期驗收器明確拒絕，沒有以任意crash當抓錯。正常四seed82checks PASS及飽和83checks PASS仍保留；mutant coverage一律排除，不進共用正常merge。

保存cpri-random-mutant-20261107/rejection.json的clean/mutant DUT SHA與log SHA、run-manifest/binary及原始log。WSL available6421MiB，隔離單工384MiB/512MiB無網路容器自動清理，原Portfolio未啟動。此補強CPRI已登記scope收斂的scoreboard敏感度，不證明任意錯誤都能抓到；SVA/covergroup仍未量測、非Minisim PASS。未推送。


## 2026-11-07 04:14 Taipei：收斂ledger與mutant證據整合

核對current全部七份coverage_convergence_evidence報告SHA、toggle數字與remaining一致，另驗CPRI clean DUT/mutant DUT/log三份內容SHA，36筆mismatch拒絕證據吻合。current CPRI另連scoreboard_mutant_evidence並明列excluded_from_normal_coverage；正常coverage未混mutant。closure-ledger-audit-20261107.json保存七項逐scope狀態，新增模擬0。

加入acceptance_reporting說明舊fully_completed固定欄位不是驗收量測；功能、coverage、UVM、Minisim各自列證據，RTL SVA/covergroup仍未量測。DBI/CXS/CPRI量測scope閉合；GPIO toggle閉合但UVM failure保留，TDM expr8/9與UVM failure保留，SPI48/50取樣點、UART line endpoint仍待審查，不改分母或自動豁免。背景只有原buildkit，未啟動其他工作。下一步回到可達holes固定seed補測／Minisim property drain，不重跑已閉合scope，未推送。


## 2026-11-07 04:44 Taipei：AVSBus random補洞準備

AVSBus baseline toggle18/28，原始十個缺口為rst_n下降沿與vout低位雙向，原bench電壓值取樣不足。avsbus-random-20261107保留同flattened DUT/SHA，擴充private原bench：固定seeds1..4、各64組合法VID32..192隨機SET/GET配對，沿原response decoder逐ACK/voltage/parity核對；原14checks保留、預期每run142checks，尚未執行不可當PASS。

附runtime reset邊沿刺激但不假設模型會清runtime狀態，另保留line22/23與expr14/16未解。plan.json記來源SHA、固定seed、分母及驗收條件；下輪先查資源再實跑，之後共用audit同DUT keys合併、記增益與plateau。新增模擬0，不重跑已閉合DBI/CXS/GPIO；未推送。


## 2026-11-07 05:14 Taipei：AVSBus合法VID隨機收斂

DUT SHA與準備紀錄一致，保留原14checks，seeds1/2/3/4各64 SET/GET合法VID32..192配對，逐ACK/voltage/parity檢查，四run每142checks全FUNC_PASS；合計256pairs（512命令）、568checks。reset只補邊沿、不假設runtime reset清狀態；WSL available6447MiB，隔離單工384MiB/512MiB無網路、tmp乾淨建置。

共用audit確認相同DUT raw keys，toggle18/28→28/28；line/expr剩餘點保持raw明列，不因toggle閉合稱完整驗收。avsbus-random-20261107保存raw/log/manifest/shared-audit，current另連toggle閉合與line/expr待審查，baseline CSV不覆寫。SVA/covergroup未量測、非Minisim PASS；隔離容器清理、其他容器未動。下一步根據remaining點選定向刺激／生成計數分類，未推送。


## 2026-11-07 05:44 Taipei：AVSBus未命中點分類／bad-start準備

相同來源emit C++，ctrl expr counter126位於root__3.cpp451-463：ctrl1先break，ctrl0才繼續且不滿足後面ctrl1計數，因此為生成的被前置break擋住之候選點；line counter156在forever coroutine返回後。remaining-analysis.json保留generated SHA/snippet/限制，原分母不刪、不自動豁免。

另counter135對應start0，是真可達錯誤幀而既有send固定start1。avsbus-bad-start-20261107已準備private send/xact可選bad_start、原四seed任務改只seed1補測，新增ERR/電壓不變及GET確認兩checks，預期144checks；尚未執行，不當PASS。DUT不改，本輪新增模擬0。下一輪查資源後實跑此定向反例並同DUT keys合併；未推送。


## 2026-11-07 06:14 Taipei：AVSBus bad-start實跑補洞

DUT不改，seed1保留原固定隨機段後加start0的SET，要求ERR且回傳電壓不變，再GET確認未寫入；144checks全FUNC_PASS。原先真可達start-bit expr命中，共用audit同DUT keys合併後expr14/16→15/16、toggle28/28維持。原四seed PASS不無故重跑。

剩ctrl expr已由生成break前置阻擋、line為forever endpoint之靜態分類，仍保留raw分母和pending review，不自動waiver／不稱全scope100%。avsbus-bad-start-20261107 raw/log/manifest/shared-audit/status齊全，current顯式連新audit並保留原baseline。WSL available6459MiB，隔離單工384MiB/512MiB無網路容器移除、其他容器未動；SVA/covergroup未量測、非Minisim PASS，未推送。


## 2026-11-07 06:44 Taipei：I2C大分母結構性缺口分類

先讀既有i2c-toggle-holes避免重複盤點，新增精確來源SHA與守衛分類。固定SLAVE_ADDR=0x50且只驅動外部port，唯一mem寫入位於cur_addr==SLAVE_ADDR守衛下；128個地址有限枚舉只允許80，其他127 bytes無寫入路徑，對應2032 directional toggle bins。bit_cnt4bit在source只宣告一次無使用，另8 bins。共2040個結構性候選，不是整數counter高位問題。

reachability-plan.json保留假設、來源SHA、128地址枚舉結果；這是靜態二態source分析、非RTL formal proof，不刪分母、不自動waiver，原24/2080維持。不能改SLAVE_ADDR跑其他配置後混算成同一elaboration closure。下一步固定seed合法write/read補mem[0x50]低位、隨機未匹配地址NACK補cur_addr、定向repeated START補restart並驗資料保留。新增模擬0，未重跑PASS，未推送。


## 2026-11-07 07:14 Taipei：I2C可達寫讀/位址random實跑

DUT與SLAVE_ADDR=0x50不改，保留原15checks，seeds1/2/3/4各16組random byte寫讀、交替STOP/repeatedSTART，另隨機錯位址NACK及contention驗收；每run96checks全FUNC_PASS，共64寫讀pairs、64錯址checks、384checks。WSL available6449MiB，隔離單工384MiB/512MiB無網路、tmp乾淨建置，模擬watchdog600us有界配合新增交易。

共用audit確認相同DUT bins，toggle24/2080→38/2080；剩toggle分類{'mem': 2032, 'bit_cnt': 8, 'restart': 2}，其他line/expr/branch結果見shared-audit，不刪2032未寫memory及8unused counter原分母。i2c-closure-20261107保存raw/log/manifest/shared-audit/measured-summary；current另連增益與結構性/剩餘缺口待審查。不是2080全部閉合、非Minisim PASS；隔離容器移除、其他容器未動，未推送。


## 2026-11-07 07:44 Taipei：I2C獨立restart觀測未驗收

加入獨立master SDA/SCL邊沿active-state監測，seed1原96資料/ACK/contention檢查無報錯，但新增預期9次repeatedSTART斷言實際觀測10，FUNC_FAIL checks97 fails1；因此本次整run未驗收、coverage不得併入正常收斂，沒有把預期值偷偷改10。可能涉及初始/noise邊界或monitor定義，尚未查明，不能先歸責DUT。

i2c-restart-observation-20261107/observation-failure.json保存expected/observed/log SHA與excluded_from_normal_coverage。下一步逐START/STOP印timestamp及SCL/SDA狀態，辨識多出的事件後再調整有證據的monitor契約，不能以task呼叫次數替代bus event。原正常38/2080與結構性分類維持不變，新增此失敗觀測run1；WSL available6414MiB、隔離單工384MiB/512MiB容器自動清理，未推送。


### 2026-11-07 08:14 Taipei — I2C repeated-START trace

Single isolated Verilator 5.052 trace reproduced the expected failure: 97 checks, 1 restart-count mismatch (expected 9, observed 10). Edge/task timestamps localize the extra active-state SDA falling edge to 47.6 us, inside the original multi-byte read (START task 40.9 us, STOP task 50.7 us), with no explicit repeated-START task there. This is localization, not a proven DUT bug: master-only SDA versus resolved bus and same-time ACK scheduling remain to inspect. No failed-run coverage was merged; accepted random evidence remains 4 seeds / 384 functional checks and 38/2080 toggle bins. Evidence: build/portfolio-extended-20261101/i2c-restart-trace-20261107/trace-analysis.json. Next acceptance: resolve the edge classification from actual bus timing, retain expected-count negative test, then rerun the corrected observer without changing the expected value merely to pass.


### 2026-11-07 08:44 Taipei — I2C ACK setup regression

The original receive task changed master ACK/NACK and raised SCL in the same simulation time slot. A private bench correction adds 100 ns of ACK/NACK setup while SCL stays low; the DUT is byte-identical. Four fixed seeds passed 97 checks each (388 total), with the independent master-SDA/SCL observer reporting the unchanged expected nine repeated STARTs in every run. The original zero-setup failure remains as negative evidence. This resolves the observed scheduling ambiguity for this observer; it does not establish a complete resolved open-drain bus protocol monitor. Exact DUT raw-point audit is saved in build/portfolio-extended-20261101/i2c-ack-setup-20261107/shared-audit.json; no waiver or full RTL closure is claimed. Next: separately validate resolved-bus observation and classify remaining fixed-address memory / unused-counter / transient-restart bins.


### 2026-11-07 09:14 Taipei — I2C gap inventory and ledger integrity

Exact remaining raw-point inventory: 2032 memory toggle bins at all 127 indices other than fixed address 0x50; 8 toggle bins on declaration-only bit_cnt; 2 restart toggle bins; 1 expression and 1 line point (2044 total). Static write-guard/index checks support the fixed-address classification, but are not formal proofs or waivers. Restart transient sampling and generated expression/line sites still require evidence. No denominator was removed. Current ledger now links the 4-seed / 388-check ACK-setup result; SHA checks passed for all nine linked VIP convergence reports. No simulation was repeated this round. Evidence: i2c-ack-setup-20261107/gap-classification.json and closure-ledger-audit-20261107-0914.json under build/portfolio-extended-20261101. Priority remains resolved-bus observer negative tests, then generated-counter investigation.


### 2026-11-07 13:15 Taipei — I2C resolved-bus validation

Private bench now computes wired-AND SDA from master release and slave drive, replacing slave-priority mux. Master releases ACK while SCL is low before the next data byte. Unmodified DUT passed four fixed seeds, 98 checks each (392 total); both master-only and resolved-bus observers count nine repeated STARTs per run. This validates two-state bus resolution and existing read/write checks, not complete electrical/four-state compliance; intentional missing/extra START negatives remain pending. Exact DUT coverage audit and generated C++ preserved in build/portfolio-extended-20261101/i2c-resolved-bus-20261107. No waiver or denominator removal.


### 2026-11-07 13:45 Taipei — I2C targeted expression closure

Generated C++ inspection showed the missing nack==0 expression counter is evaluated after the loop body send_byte, so the existing two-byte ACK/NACK read never hit it. Added a third-byte read with ACK on the second byte, preserving the DUT. Actual seed-1 run passed 99 checks, including third byte value and both nine-repeated-START observers. Exact-key merge with four accepted resolved-bus runs adds one raw point: expr 10/10, branch 10/10; toggle remains 38/2080 and raw line 11/12. No denominator removed or waiver added. Evidence and generated code: build/portfolio-extended-20261101/i2c-three-byte-20261107/shared-audit.json. Remaining memory/unused counter/transient restart/line gaps are not full RTL closure.


### 2026-11-07 14:15 Taipei — I2C remaining attribution

Saved generated C++ snippets, line numbers and SHA for restart transient assignment/clear and separate toggle sampling. The remaining line counter 2199 is after the unconditional forever loop before co_return. These are static tool-attribution findings, not a formal proof or waiver. Raw denominators remain intact: toggle 38/2080, line 11/12, expr 10/10, branch 10/10. Current ledger now links the three-byte supplement and attribution report. No simulation repeated and no failed/mutant coverage merged. Next useful dynamic work is independent observer missing/extra START negatives rather than additional blind random seeds.


### 2026-11-07 14:45 Taipei — actual START observer negatives

Built one private I2C bench with two stimulus modes. MISSING_START replaces one repeated START with STOP+START: both observers count 8 versus unchanged expected 9. EXTRA_START appends an idle START/repeated-START/STOP sequence: both count 10. Each actual run executes 99 checks and fails exactly the two named restart-count checks; all other checks pass. Python requires the intended failure messages/counts and successful simulator execution, so crash/timeout cannot satisfy expected rejection. DUT hash matches the clean three-byte run. Both failed-run coverage datasets remain excluded from normal coverage. Evidence: build/portfolio-extended-20261101/i2c-start-negatives-20261107/negative-results.json; current ledger links its SHA. This validates event-count detection, not all I2C protocol rules/electrical compliance.


### 2026-11-07 16:15 Taipei — reject corrupt coverage records

The raw-point parser silently skipped malformed C records; the same corrupted point in baseline and run could shrink both denominators without a key mismatch. It now rejects malformed/truncated C records. Twelve regression methods passed, including four invalid-count variants and truncation. Six saved real I2C raw files retain identical 2112 DUT points across categories. No simulation repeated or historical report rewritten. Evidence: build/portfolio-extended-20261101/coverage-parser-validation-20261107.json. This is syntax validation, not source equivalence or RTL closure.


### 2026-11-07 22:15 Taipei — isolated DigRF measurement

Safer alternative executed the existing single-VIP runner directly in a new --rm container; original stopped portfolio container and its RAM/swap settings were untouched. First attempt lacked flattened sources and is retained as preparation error, not IP failure. After read-only copying the pinned flattened sources, DigRF completed in 164.72 s with FUNC_PASS checks=20; CSV tests 1/1, line 30/30, toggle 155/198, expr 17/17. SVA and functional covergroups 0/0 remain unmeasured, not closure. Exact ZIP matched local pinned archive; checkpoint source SHA fbee3655c9f559d6efdebe21b389304244a2d18c4be8a6dd3baf9e909dd60a49. Evidence/result SHA linked in isolated_single_vip_evidence, without rewriting historical progress/CSV. This is Verilator 5.052, not Minisim/UVM acceptance. Next: inspect 43 toggle holes for reachable stimulus.


## 00:39 Taipei — DigRF fixed-seed convergence

Unmodified pinned flattened DUT, four seeds with 32 random 1..64-byte frames each. All runs passed 52 functional checks (208 total, including original20 per run); added128 random frames checked by the existing CRC reference and trimmed queue length. Exact raw DUT point keys/source SHA audited before merge; gains per seed and remaining holes saved in build/portfolio-extended-20261101/digrf-random-20261108/shared-audit.json. Baseline remains historical, no waiver or denominator removal. This is Verilator reference coverage, not Minisim/UVM or complete DigRF protocol acceptance.


## 05:59 Taipei — DigRF reachable raw holes closed

Actual maximum word_count=65535 frame followed by a four-byte frame passed CRC and queue-length checks, 54 checks total including original and random traffic. Private send_frame computes its invariant reference CRC once per frame instead of every beat, avoiding quadratic work; DUT bytes unchanged. Same-key merge adds remaining18 toggle points: 198/198, expr17/17, raw branch22/22, raw line8/8 (expanded baseline line30/30); remaining raw list empty. No waiver or denominator removal. Evidence: build/portfolio-extended-20261101/digrf-long-frame-20261108/shared-audit.json and seed-1/run.log. This closes measured raw code scope only; RTL SVA/functional covergroups/UVM/Minisim and full DigRF protocol compliance remain unaccepted. Next high-value check is an actual CRC DUT mutant against this same oracle.


## 06:29 Taipei — DigRF actual CRC DUT mutant rejected

One private DUT expression flips bit0 of calculated CRC. Actual run completed54 checks with 38 failures, including random-frame and maximum-word-count CRC/length checks; expected rejection requires both named messages, nonzero check failures, normal simulator exit and no timeout/PASS. Clean and mutant DUT SHA saved with exact single-change check. Evidence: build/portfolio-extended-20261101/digrf-crc-mutant-20261108/rejection.json. Failed raw coverage is excluded; the clean 198/198 toggle closure remains separate. This demonstrates oracle sensitivity to this CRC mutant, not complete protocol correctness.


## 07:29 Taipei — DisplayPort2 isolated baseline

Next unmeasured VIP executed via existing single-VIP runner in a fresh container, source snapshot copied to its own filesystem. Corrected preparation-only absolute manifest path/CRLF bootstrap issues, retaining first failed run. Final run completed23.88s: original functional16 checks, xfail0, dut_errors6 intentionally provoked timing violations (all expected counter checks), line26/26, toggle210/486, expr20/20. SVA/functional covergroups0/0 unmeasured; no UVM or Minisim acceptance. Original stopped container untouched. Evidence: build/portfolio-extended-20261101/displayport2-isolated-20261108/result.json. Next: group276 toggle holes before choosing bounded random/targeted stimulus; preserve baseline.


## 08:46 Taipei — DisplayPort2 fixed-seed convergence

Unchanged sink DUT, four seeds each64 legal model-contract MSA and64 SDP events,145 checks/run (580 total). Timings are random24-bit values ordered to meet this educational model active<=total and sync_start<=sync_end; not certified display modes. Independent counter checks require exactly one new frame/no timing errors and correct SDP/PSR increments. All real runs pass; original16 checks and six intentional errors remain. Exact-key/source-SHA audit saved in build/portfolio-extended-20261101/displayport2-random-20261108/shared-audit.json; no waiver or denominator removal. Random lane bytes are input toggles under video data, not proof of full lane decoding. Verilator result only, not Minisim/UVM/SVA acceptance.


## 09:16 Taipei — DisplayPort2 real timing mutant rejected

A single private DUT comparison hactive>htotal was reversed to <. Actual seed1 run completed145 checks, 72 failed including named random legal-MSA/no-timing-error checks; expected rejection requires normal simulator completion, no timeout and that specific failure. Source single-change and clean/mutant SHA verified. Evidence: build/portfolio-extended-20261101/displayport2-timing-mutant-20261108/rejection.json. Mutant raw coverage excluded; normal486/486 toggle result remains separate. This demonstrates sensitivity to a timing comparator bug, not full DisplayPort conformance or Minisim acceptance.


### 2026-11-08 — next unmeasured Ethernet intake

Read-only archive inspection confirms the existing Ethernet functional bench already exercises 40 randomized frames, 512-bit beats and eight-byte BE units. Do not present additional random traffic as a new capability or claim arbitrary byte-length support. Before stimulus changes, run the pinned original baseline in a fresh isolated single-target container after memory/job checks, retain atomic CSV/raw evidence, then audit exact DUT point keys and per-seed gains. Follow with reachable-hole supplements and an actual CRC mutant. Acceptance plan and archive/bench SHA: ../helios-verify/build/portfolio-extended-20261101/ethernet-intake-plan-20261108.json. This round is intake design only: no attempted simulation, coverage gain or Minisim PASS. Original stopped container remains untouched.


### 2026-11-08 — Ethernet preflight infrastructure unavailable

Read-only preflight found WSL available memory 6963 MiB and unused swap, but Docker Desktop Linux engine API pipe was absent. Both container inventory and original-container inspect failed to connect: current container state is unobservable, not newly classified as exited/running. No Ethernet simulation was attempted; no IP failure/timeout or coverage update is recorded. Did not start Docker Desktop, the old container or controller. Evidence: ../helios-verify/build/portfolio-extended-20261101/ethernet-preflight-20261108.json. Next: once Docker API is available, recheck jobs/resources and execute the isolated pinned baseline. Do not silently substitute WSL Verilator5.032 for portfolio5.052.

### 2026-11-10 — isolated Ethernet baseline compile attempt

Docker was reachable again. The original portfolio container remained
exited. Its 28 Ethernet RTL and header files matched the pinned ZIP
after LF normalization. The isolated run used the existing image with
Verilator 5.052, one CPU, 384 MiB RAM, 512 MiB including swap, no network
and the established single-worker coverage profile. Windows had about
1.3 GiB free at preflight. The isolated container was removed on exit.

The atomic CSV recorded two discovered functional benches and zero
covered tests. Both plain flattened builds failed on the unresolved
`net_link_state_e` cast before simulation. No timeout occurred. Every
coverage denominator is zero and unmeasured. The packaged flattened model
imports `net_vip_pkg`, and the saved plain command contains no package
sources. Next, verify package order and dependencies in a bounded
lint-only probe, then use a fresh attempt if elaboration succeeds.
Keep the first attempt classified as a compile failure. No UVM or
Minisim result is inferred. Source and command hashes, the raw CSV and
compiler logs are in
`../helios-verify/build/portfolio-extended-20261101/ethernet-isolated-20261110/`.

### 2026-11-10 — Ethernet package-aware elaboration

The first isolated snapshot omitted `smoke/smoke_run.py`.
`run_func_tests.vip_pkgs()` catches the missing-helper exception and
returns an empty package list, leaving only the plain build. Preserve
the first CSV and compiler logs. Classify that attempt as an incomplete
isolation snapshot, with no attributable functional result for the
complete Ethernet flow.

The helper was recovered read-only from the stopped portfolio container
and matched the local upstream file after LF normalization. With that
helper, the existing runner generated its package-aware compile inputs.
Both original benches passed Verilator 5.052 lint and elaboration using
the `pkgs` candidate, with the sanitized VIP and DUT CRC packages and
`xchk_ref_pkg` before the generated DUT and original testbench. Logs,
commands and helper SHA are in
`../helios-verify/build/portfolio-extended-20261101/ethernet-package-lint-20261110/`.
This was two lint checks and zero simulations. The original container
remained exited. Next, recheck resources and run the normal isolated
coverage driver with the restored helper in a fresh attempt. Require
nonempty per-bench functional verdicts and attributable CSV/raw data.

### 2026-11-10 — Ethernet attempt prepared, launch deferred

Verified the hashes of 28 normalized sources, 10 flattened files,
the restored helper and the coverage driver against the saved inputs.
The prepared attempt uses the same pinned image and single-worker
profile, a read-only snapshot and helper, and a fresh evidence output.
Its command, bootstrap and request are under
`../helios-verify/build/portfolio-extended-20261101/ethernet-baseline-prepared-20261110/`.

Windows free memory was 334280, 421156 and 470348 KiB across three
observations. WSL reported 6525 MiB available on the second check.
The full run stayed below its 512 MiB Windows launch guard, so no
isolated test container or simulator was launched. This is a preflight
deferral, with no new attempt, timeout or coverage measurement. Next,
recheck both memory pools, Docker jobs and the original container state,
then use the prepared command when resources allow.

### 2026-11-10 — Ethernet baseline executed

The corrected single-worker Verilator 5.052 run exited normally in
36.4 seconds with no timeout. Both original benches passed.
`smoke_func_tb` reported 19 checks and `func_ethernet_tb` reported 18.
The atomic CSV records 2/2 covered tests, line 47/49, toggle 1071/1082
and expression 21/24. SVA and functional covergroup are both 0/0
unmeasured. The earlier incomplete-snapshot attempt is retained.

The run used the pinned image with 384 MiB RAM, 512 MiB including swap,
one CPU and no network. Windows preflight free memory was 706840 KiB
and WSL available was 6515 MiB. The isolated container was removed and
the original portfolio container stayed exited. Evidence is in
`../helios-verify/build/portfolio-extended-20261101/ethernet-baseline-prepared-20261110/`.
Next audit the DUT point keys and the per-bench contribution before
designing stimulus for reachable holes. This result covers the two
original functional benches under Verilator. UVM, Minisim and full
coverage closure remain separate.

### 2026-11-10 — Ethernet point attribution and directed cases

The shared raw parser read 1155 DUT points per bench and 2310 in the
merged file. The merged keys and hit counts equal the two input files
combined. Normalizing hierarchy and package-copy filenames with the
upstream design-unit rule reproduces line 47/49, toggle 1071/1082 and
expression 21/24. The CSV line category includes line and branch
points. The feature bench contributes 128 exclusive hits (126 toggle,
one line and one expression). The smoke bench contributes no exclusive
hit in this measured scope. Both functional verdicts remain separate.

The two line, eleven toggle and three expression holes are retained in
`../helios-verify/build/portfolio-extended-20261101/ethernet-baseline-prepared-20261110/coverage-point-audit.json`.
Start with a valid 120-byte frame plus preamble over two full beats.
Drive BE FF then 00 and check one FCS pulse, a good independent CRC
verdict and no length or CRC error. This targets BE bit 0 falling and
the zero-BE expression. Separately, use a reset-cleared frame queue
and send an accepted EOP without SOP after link recovery. Check the
model's length-error and FCS-valid pulses with CRC not OK. Compare the
exact size-related point keys after this probe, including the PAUSE
expression whose other terms may prevent a hit.

The default arm of the four-state-encoding link FSM and high link-state
bits are structural candidates under the measured two-state simulator.
The FCS, framing and FEC error outputs appear low-only in this model.
Retain these points and review their drivers before any reachability
disposition. No waiver, new simulation, coverage increment or container
launch occurred in this audit.

### 2026-11-10 — Ethernet directed bench prepared

A private copy of the feature bench preserves its original 18 checks
and adds five checks for link recovery, a good full-width final beat
with BE 00, and length/error pulse behavior on an orphan EOP after
reset clears the queue. The original DUT and upstream testbench are
unchanged. The isolated bootstrap selects only the modified feature
bench. Its shell syntax check passed.

Windows free memory was 387156 KiB, below the 512 MiB launch guard.
No container, simulator or coverage collection started. The prepared
bench, bootstrap, request and source fingerprints are in
`../helios-verify/build/portfolio-extended-20261101/ethernet-directed-20261110/`.
Next recheck resources and require 23 passing checks, exact DUT point
key agreement and a separate report of newly hit points. Keep the
original baseline intact.

### 2026-11-10 — resume inventory includes three isolated baselines

The resume planner now includes the verified baseline CSVs for DigRF,
DisplayPort2 and Ethernet in
`../helios-verify/build/portfolio-extended-20261101/vip-coverage-resume-plan-20261110.json`.
Use this inventory for the next selection. The previous plan and its
21 selected CSVs were preserved, and all 24 selected CSV hashes and the
recovered partial report hash were checked. The inventory has 130 VIPs,
24 measured checkpoints, four recovered partial records and 102 with
no recorded coverage. All 130 remain pending full acceptance. Existing
UVM retry classifications are unchanged. The first Ethernet attempt
remains separately recorded as an incomplete isolation snapshot.

Windows free memory was 279324 KiB at the directed-test preflight.
No container or simulator was started. Resume the prepared Ethernet
directed test once the memory and job checks allow it. This inventory
update did not add any new dynamic measurements.

### 2026-11-10 — Ethernet directed checks executed

After the resource preflight passed, one isolated Verilator 5.052
run completed in 26.23 seconds with 23 passing checks, no xfails and
no timeout. The generated DUT text and all 1155 raw feature-bench DUT
point keys match the baseline. Five new points hit and no prior hit
was lost. The new points cover BE bit 0 falling, the zero-BE term,
the short-queue FCS branch and expression, and the PAUSE size term.
The CSV reports line 48/49, toggle 1072/1082 and expression 24/24.
SVA and functional covergroups remain unmeasured.

CSV, log, raw data, exact new points and hashes are under
`../helios-verify/build/portfolio-extended-20261101/ethernet-directed-20261110/`.
The isolated container was removed and the original stayed exited.
The original two-bench baseline remains the resume checkpoint.
Next, review the remaining one line and ten toggle holes for
structural reachability, and verify a genuine CRC DUT mutant with the
existing independent frame oracle. This result is scoped to the
Verilator feature bench.

### 2026-11-10 — Ethernet CRC mutant rejected

A private DUT copy inverts one CRC comparison from inequality to
equality. The elaborated and annotated DUT text confirms that single
change. The directed bench is byte-identical to the 23-check clean
run. The mutant reached normal simulator finish in the isolated
Verilator 5.052 flow and failed six checks, including the original
corrupted-FCS verdict and the new BE 00 good-CRC check.

The coverage runner returned zero and wrote a CSV with one covered test
because the failing run emitted coverage data. The functional log and
CSV detail both show failure. The mutant data is excluded from normal
coverage and from the resume inventory. The first WSL memory query
timed out before launch. A later query succeeded and the run completed
in 13.89 seconds without timeout. The isolated container was removed
and the original stayed exited. Evidence is in
`../helios-verify/build/portfolio-extended-20261101/ethernet-crc-mutant-20261110/`.
Next review the remaining structural holes without changing denominators
or assuming waivers.

### 2026-11-10 — Ethernet remaining points reviewed

All eleven remaining raw points have a source explanation in
`ethernet-directed-20261110/reachability-review.json`. The two-bit link
FSM uses every binary encoding, leaving its default arm outside normal
two-state execution. The cast to a four-bit link state zero-extends
the unsigned state, leaving four high-bit transition points low-only.
FCS error, framing error and FEC uncorrectable are assigned zero only,
accounting for six more transition points.

This is a source review for the measured Verilator model after reset
under normal port stimulus. It does not establish reachability under
four-state simulation or fault injection. There are zero waivers.
Preserve line 48/49, toggle 1072/1082 and expression 24/24, with SVA and
covergroups unmeasured. Positive and mutant result hashes verified
and no simulator was rerun. Next select another unmeasured portfolio
VIP when the resource checks permit. Keep the Ethernet evidence in its
existing functional and coverage scopes.

### 2026-11-10 — I2S selected for the next isolated baseline

I2S is unmeasured in the refreshed 130-VIP resume inventory. Its
original `func_i2s_tb.sv` is in the functional lane for the pinned
snapshot. The 24 RTL and header files copied read-only from the
stopped portfolio container match the archive after LF normalization.
The nine flattened files and manifest are fingerprinted. The prepared
command reuses the pinned Verilator 5.052 image, restored smoke helper
and existing single-worker profile.

Windows free memory was 247804 KiB on the launch check, below the
512 MiB guard. No container or simulator started. The prepared
source and command are in
`../helios-verify/build/portfolio-extended-20261101/i2s-isolated-20261110/`.
At launch recheck available memory, Docker jobs and input hashes.
Require a nonempty functional verdict and atomic CSV, preserve raw
coverage and compile diagnostics, and report the functional lane
separately from UVM and Minisim.

### 2026-11-10 — I2S acceptance contract checked

The pinned original I2S bench has 14 check sites covering 24-bit left
and right capture, adjacent frames, short-slot rejection, readback,
loopback and reset recovery. Its loopback check requires more than
100 samples and zero mismatches. The only flattened DUT in the
manifest is `i2s_receiver_loopback`. The acceptance plan beside the
prepared command pins the bench and manifest. The next execution
must provide a clean 14-check functional verdict and attributable
nonempty DUT coverage. A CSV covered-test count alone is insufficient.

Windows free memory was 222300 and 320068 KiB on the two checks.
No new simulation was launched and no coverage was measured.

### 2026-11-10 — I2S baseline executed

The original I2S bench passed all 14 checks in an isolated Verilator
5.052 run, including its loopback condition of more than 100 samples
and zero mismatches. The single-worker flow completed in 20.61 seconds
without timeout. Its atomic CSV and exact raw DUT counts agree on line
23/26, toggle 110/112 and expression 12/13. SVA and functional
covergroups remain unmeasured. Six remaining point keys are recorded.

Windows had 642432 KiB free and WSL had 6541 MiB available at
preflight. The isolated container was removed and the original
portfolio stayed exited. Logs, CSV, raw data and result fingerprints
are in
`../helios-verify/build/portfolio-extended-20261101/i2s-isolated-20261110/`.
Next run the prepared loopback mutant separately and retain its
functional failure outside the normal coverage baseline. The clean
result is for the Verilator functional lane.

### 2026-11-10 — I2S loopback mutant prepared

The private flattened DUT inverts the serial loopback on the falling
SCK edge at one checked site. The original 14-check testbench and
clean artifacts are preserved. The isolated command, source hashes
and named expected failure are in
`../helios-verify/build/portfolio-extended-20261101/i2s-loopback-mutant-20261110/`.

WSL reported 6486 MiB available and Windows reported 43672 KiB free.
The Windows preflight prevented launch. No mutant simulation ran.
When resources allow, require normal finish and the named loopback
check to reject the mutant. Keep the mutant's coverage out of the
clean resume inventory.

### 2026-11-10 — I2S baseline added to resume inventory

Use `../helios-verify/build/portfolio-extended-20261101/vip-coverage-resume-plan-i2s-20261110.json`
for the next selection. Verified the 25 selected CSV hashes and the
recovered partial report. The 130 VIPs now have 25 measured checkpoints,
four recovered partial records and 101 with no recorded coverage.
Full acceptance and UVM retry classifications are unchanged.

Windows had 249088 KiB free. The I2S loopback mutant remains prepared
and unattempted. The inventory adds only the existing clean I2S baseline.

### 2026-11-10 — I2S remaining point review

The six baseline holes map to three directed targets and three
structural candidates. Test absent left/right readback and a 25-bit
left slot followed by a valid right slot. For absent indices, check
that reading does not allocate an entry and existing samples are
preserved. A two-state run cannot establish an X-valued result.
The other points are the inactive-left term at a rising WS edge and
both transitions of rshift bit 23. The latter remains low after
two-state initialization because the last bit is appended directly
to right_mem. Keep the raw denominators and zero waivers.

The plan is in `i2s-isolated-20261110/point-followup-plan.json`.
Windows had 102800 KiB free at the final launch check. No mutant or
directed simulation was started. Run the isolated loopback mutant
first once the resource gate allows it.

### 2026-11-10 — I2S directed bench prepared

A private copy preserves the original 14 check sites and adds four
for missing-index readback and a 25-bit left slot followed by a valid
right sample. The missing values are logged without an X assertion in
the two-state simulator. The extra bit is driven explicitly to avoid
indexing beyond the original 24-bit stimulus. The DUT and original
bench are unchanged. The candidate and its fingerprints are in
`../helios-verify/build/portfolio-extended-20261101/i2s-directed-20261110/`.

Windows free memory was 443536 KiB. No container or simulator ran.
Execute the prepared loopback mutant first when resources allow.
The directed acceptance gate is 18 passing checks and exact DUT
point identities, with the three target points reported separately.

### 2026-11-10 — I2S structural candidates checked in a finite model

A source-derived abstraction explored 94 states and 376 transitions
from zero-initialized rshift under binary WS and reset. Every sampled
rising WS edge had an active left slot. Right shift storage reached
at most 23 data bits because the final bit goes directly to right_mem.
The report is `i2s-isolated-20261110/point-state-review.json`. This
supports the three structural candidates within those assumptions.
Coverage denominators and waiver counts are unchanged.

Windows had 274956 KiB free. No RTL simulator or mutant container
started.

### 2026-11-10 — coverage checkpoint functional failure classification

The Helios resume planner now labels the upstream
`FAIL fails under coverage` diagnostic as a functional failure under
coverage and retains review status. The measured counts remain
available. Eight planner unit tests passed. The saved Ethernet
positive and CRC mutant CSVs both say one covered test, but only the
mutant was flagged. Neither file was added to the selected inventory
during this check. Evidence is in
`checkpoint-functional-failure-classification-20261110.json`.

Windows free memory dropped to 10308 KiB before launch. No I2S
mutant or other simulator was started.

### 2026-11-10 — I2S loopback mutant rejected

The isolated Verilator 5.052 run completed in 16.09 seconds with no
timeout. The original bench completed all 14 check sites and reported
one failure, the half-cycle serial loopback check. The annotated
testbench matched the clean run. The annotated DUT differed only at
the intended serial output inversion. The simulator reached finish.

The coverage runner returned zero and recorded one covered test
because the failing run produced data. Its detail and functional log
show the failure. Mutant coverage is excluded from the clean
inventory. Evidence is in
`../helios-verify/build/portfolio-extended-20261101/i2s-loopback-mutant-20261110/`.
The isolated container was removed and the original stayed exited.
Next run the prepared directed bench on the original DUT.

### 2026-11-10 — checkpoint compile failure distinguished

The Helios resume planner now labels the upstream functional compile
diagnostic separately from a functional failure after simulation.
Nine unit tests passed. Saved CSVs verified three outcomes. The
incomplete Ethernet snapshot is an unmeasured compile failure, the
I2S mutant is a measured functional failure, and the I2S clean row
has neither issue. The compile label does not attribute a root cause
to the DUT. The selected inventory was not changed. Evidence is
`checkpoint-compile-failure-classification-20261110.json`.

Windows had 99168 and 327976 KiB free on the two final checks.
The I2S directed run did not start.

### 2026-11-10 — I2S directed acceptance preflight

The four prepared launch inputs and the baseline result match their
recorded SHA256 values. The directed bench contains 18 static check calls.
The saved baseline has 151 exact raw DUT point keys, with six zero counts
matching the follow-up plan. Accept the directed run only after all 18
checks pass and the same 151 DUT keys are present. Verify each of the three
target keys independently (absent left read, absent right read, and the
25-bit left-slot branch). Keep any residual zero counts visible and
exclude the loopback mutant from the merge.

Windows reported 42180 KiB free, below the 524288 KiB launch guard. No
container or simulator was started for this preflight. Run the prepared
single-VIP attempt once the resource checks pass.

Helios now includes `new_point_keys` per round in the shared convergence
report, alongside the existing gain count. Compare those keys directly
against the I2S target keys once the run is available. The 13 unit tests
passed. Replaying the saved Ethernet feature-bench data reproduced all
five previously recorded gain keys, with 1155 DUT points and 11 unhit.
Use per-bench data for that replay because the baseline merged data also
contains another bench. No new simulation was run. Windows reported
363764 KiB free and the I2S run stayed below the launch guard.

### 2026-11-10 — I2S long-slot negative control design

The directed bench drives 24 payload bits and one extra low-WS sample
before the transition to the right slot. The model counts the first
sample on the WS edge, then increments on each further left sample.
At the transition, the original DUT requires exactly 24. A private
single-site mutation from equality to greater-than-or-equal should
preserve 16-bit rejection and 24-bit capture, and incorrectly store
the 25-bit sample at index 5. A source-derived check of the three
lengths confirms that expected discrimination.

After the clean 18-check run passes, accept the negative control only
if the mutant reaches normal finish with the named 25-bit storage
check failing and the right sample/index check passing. Preserve the
DUT diff and log separately. This is a test design with no simulator
attempt. On the last launch preflight Windows free memory fell to
79092 KiB, while WSL reported 6516 MiB available.

### 2026-11-11 — I2S directed functional replay in WSL

Verilator 5.032 compiled the saved flattened DUT and prepared directed
bench with one build job. Both input hashes matched their source records.
The clean run passed all 18 checks. The single-site length comparison
mutation reached normal finish with exactly one failure, the named
25-bit left-slot storage check. The right-channel sample and frame
index passed in both runs.

Build logs, simulation logs, input hashes and commands are in
../helios-verify/build/portfolio-extended-20261101/i2s-directed-wsl-20261111.
The runs used functional simulation without coverage instrumentation.
The Docker Linux engine endpoint was absent at preflight. The
Verilator 5.052 checkpoint and three raw point gains remain pending.

### 2026-11-11 — I2S coverage replay in WSL

Verilator 5.032 rebuilt the original and directed benches with the same
flattened DUT and line/toggle instrumentation. The builds verified all
three input hashes. The baseline passed 14 checks and the directed run
passed 18. The two runs have identical sets of 78 raw DUT point keys.
The directed run newly hits the long left-slot branch at line 69. The
union reaches 10/10 raw line, 12/12 branch and 55/56 toggle points.

Verilator 5.032 rejected the initial coverage-expression option before
simulation. The accepted replay used line and toggle flags. Its point
set differs from the 151-key Verilator 5.052 baseline. The pinned
checkpoint and the two absent-readback point gains still need 5.052
verification. Logs, data and audit are in
../helios-verify/build/portfolio-extended-20261101/i2s-coverage-wsl-20261111.
Docker's Linux endpoint was absent, and both WSL runs finished.

### 2026-11-11 — I2S coverage key comparison across Verilator versions

The saved original-bench data has 78 raw DUT keys in 5.032 and 151
in 5.052. For this comparison only, omit the 5.052 type field and
collapse the two directional toggle keys with a logical OR of hit
status. All 78 older keys then match, with zero hit-status differences.
The 5.052 data also has four conditional branches and 13 expression
keys beyond the normalized overlap. The absent left and right read
branches at lines 28 and 31 have no 5.032 points. The long-slot branch
at line 69 maps to the point gained in the 5.032 directed run.

Inputs were i2s-coverage-wsl-20261111/baseline.dat and
i2s-isolated-20261110/raw/I2S-VIP/merged.dat under the Helios extended
build directory. This was an offline comparison with zero simulator
runs. Keep raw directional counts for the 5.052 checkpoint. Once the
pinned tool is available, verify all three target keys directly.
