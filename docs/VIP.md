# Verification IP on minisim

Two sets of UVM verification IP run on `msim -uvm`:

- the **in-tree library** in `vip/` (AXI4, AXI4-Lite, APB4 and a memory
  model), tested in CI;
- **open-vip-portfolio**, 146 protocol VIPs kept in their own repository,
  run on msim with `tools/mvip.py`.

Both need the Accellera UVM library, as `docs/UVM.md` describes
(`export UVM_HOME=<uvm-core checkout>`).

## The in-tree library

`vip/README.md` lists the VIPs. Each has a self-test bench in `tests/vip/`,
where a master agent talks to the matching slave agent with no DUT between
them, so the two sides, both monitors, the protocol assertions and the
memory model are checked against each other and against a shadow model:

```sh
msim -uvm -f tests/vip/amba/files.f -top vip_tb_top +UVM_TESTNAME=vip_axi4_loopback_test
msim -uvm -f tests/vip/amba/files.f -top vip_tb_top +UVM_TESTNAME=vip_axi4_outstanding_test
msim -uvm -f tests/vip/amba/files.f -top vip_tb_top +UVM_TESTNAME=vip_axi4_excl_test
msim -uvm -f tests/vip/axi4w/files.f -top vip_tb_top +UVM_TESTNAME=vip_axi4_loopback_test
msim -uvm -f tests/vip/axi4_sva/files.f -top axi4_sva_tb +UVM_TESTNAME=axi4_sva_neg_test
msim -uvm -f tests/vip/amba/files.f -top vip_tb_top +UVM_TESTNAME=vip_apb_loopback_test
msim -uvm -f tests/vip/axil/files.f -top axil_tb    +UVM_TESTNAME=axil_loopback_test
```

(run from the repository root). A bench passes when UVM reports no error and
it prints `SB_PASS checks=<n>`. `ctest -L vip` runs them all; a new bench is
a directory `tests/vip/<name>/` with a `files.f` whose `//! top:` line names
the top module and whose `//! test:` lines name the UVM tests.

To use a VIP in a testbench, compile its interface, assertion module and
package after `vip/mem/mem_vip_pkg.sv`, put an agent in the environment,
and set its configuration object and virtual interface:

```systemverilog
axil_cfg cfg = axil_cfg::type_id::create("cfg");      // AXIL_MASTER by default
uvm_config_db#(axil_cfg)::set(this, "bus", "cfg", cfg);
bus = axil_agent::type_id::create("bus", this);
// in the top module:
uvm_config_db#(virtual axil_if)::set(null, "uvm_test_top.*", "vif", bus_if);
// in a test:
axil_api::write(env.bus.sqr, 32'h40, 32'h1122_3344, resp);
```

### AXI4

`vip/axi4` follows AMBA AXI and ACE Protocol Specification IHI0022H.c for
the AXI4 interface; `spec/README.md` maps each requirement of the
specification to the code and the test that covers it.

- **Signals**: the whole AXI4 set, AxQOS, AxREGION and the five xUSER
  signals included. Widths are fixed per compilation by
  `vip/axi4/axi4_defines.svh` (`+define+AXI4_DATA_W=128`, `AXI4_ADDR_W`,
  `AXI4_ID_W`, `AXI4_USER_W`; defaults 32/32/8/8). Code outside the
  package reads them as `axi4_pkg::AXI4_DATA_WIDTH` and so on.
- **Master**: `cfg.max_outstanding` bursts in flight (default 1: finish_item
  returns with the response, as before). Above 1, finish_item returns when
  the driver has taken the burst and `item.done` marks the response;
  `axi4_api::run` waits for it. W may lead AW; responses are matched by ID.
- **Slave** (memory responder): `cfg.reorder` answers different IDs out of
  order, `cfg.interleave` interleaves read beats of different IDs, an
  exclusive monitor per ID answers EXOKAY (`cfg.exclusive_support = 0` for
  a slave without one), BUSER/RUSER echo AxUSER, `err_ranges` give
  SLVERR/DECERR.
- **Sequences**: `axi4_random_seq` makes legal bursts with random AxCACHE
  (Table A4-5 values), AxPROT, QoS, region and user values, `excl_pct`
  exclusive pairs, and holds back a burst that overlaps one in flight;
  `axi4_api::excl_read` / `excl_write` run an exclusive pair.
- **Checker**: `axi4_sva` checks 48 rules (reset, handshakes, X, burst
  legality, 4 KB, AxCACHE, exclusive restrictions, WSTRB lanes, WLAST/RLAST,
  B after the write, R after the read, EXOKAY use) and reports each as
  `[<section> <RULE>]`; `MAX_WAIT` adds warnings for a READY that stays
  low. `tests/vip/axi4_sva` breaks each of the 48 rules once (and
  triggers one wait warning) and checks that exactly the expected rules
  fire.

Benches: `tests/vip/amba` (32-bit; loopback, outstanding with reordering
and interleaving, directed exclusive cases), `tests/vip/axi4w` (the same
tests on a 128-bit data, 40-bit address bus), `tests/vip/axi4_sva`.

Not covered: AXI3, AXI5 and ACE signals; the slave is a plain memory
without the memory-type behaviour of A4.

The AXI4, APB4 and memory VIPs are those of SoC-tb-regress-builder. Two
names were changed: `axi4_monitor::flush` is `flush_state` and the bench's
`check` is `chk`, because `uvm_component` already has virtual `flush()` and
`check()` methods with other signatures (8.20: an override must match).

## open-vip-portfolio on msim

```sh
git clone https://github.com/tonythetiger168/open-vip-portfolio
python3 tools/mvip.py --portfolio open-vip-portfolio --md vip_results.md
python3 tools/mvip.py --portfolio open-vip-portfolio UART-VIP SPI-VIP    # some of them
```

`mvip.py` unpacks the VIP zips and takes each VIP's file list, top module,
tests and pass rule from the portfolio's own runner
(`tools/run_uvm_tests.py`); it only replaces the simulator, running
`msim -uvm --single-unit ...` where that runner builds a Verilator binary.
`--single-unit` makes the files one compilation unit, as Verilator compiles
them: the VIPs rely on a macro or `$unit` class of an earlier file.

Results per test: `PASS`; `WARN` (no error, but the VIP's own checkers
warned); `FAIL` (a UVM error, a timeout, or no pass marker); `ERROR` (msim
could not compile or lower the VIP); `NONE` (the VIP has no UVM test).
`--csv` saves a run and `--expect tests/vip/portfolio_baseline.csv` compares
with the saved one below, failing only when a test that passed there no
longer does. Logs are in
`mvip_work/logs/`.

### Results

Run of 2026-11-10 against open-vip-portfolio v1.10.0 (148 tests of 146 VIPs;
saved as `tests/vip/portfolio_baseline.csv` for `--expect`):

| Result | Tests |
|---|---|
| PASS | 74 |
| WARN | 13 |
| FAIL | 53 |
| ERROR | 6 |
| NONE | 2 |

Of the 72 checked tests (register models and template-built agents, each
with a scoreboard pass marker) 64 pass. The smoke tests have no pass marker,
and many of them fail on Verilator as well (the portfolio's
`docs/UVM_SMOKE.md` lists those): a scoreboard mismatch there is a VIP
finding, not a simulator one. What is known to be msim's:

- `rand` variables wider than 64 bits are not supported (DDR5, HBM, HBM3E, JTAG);
- a caller's array read in `randomize() with {...}` is not supported (I3C);
- HDMI stops in lowering with an internal error.

msim is a 4-state simulator and Verilator a 2-state one, so signals a VIP
never drives are x here: the AMBA zips leave the AHB signals of an APB or
AXI bench undriven and their own `HTRANS` X assertion fires.

| VIP | Test | Kind | Result | Checks | First problem |
|---|---|---|---|---|---|
| ACE-UVM-VIP | `ace_func_test` | checked | PASS | 217 |  |
| ACE_Lite-UVM-VIP | `ace_lite_func_test` | checked | PASS | 118 |  |
| AHB-VIP | `amba_ral_test` | checked | PASS | 1586 |  |
| APB-VIP | `amba_ral_test` | checked | FAIL |  | rc=0 UVM_ERROR=26415 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR rtl/amba_vip_if.sv(181) @ 105000: reporter [AMBA_IF] AHB HTRANS invalid |
| ATB-UVM-VIP | `atb_func_test` | checked | PASS | 92 |  |
| AVSBus-VIP | `avsbus_random_test` | smoke | PASS |  |  |
| AXI-VIP | `amba_ral_test` | checked | FAIL |  | rc=0 UVM_ERROR=13052 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR rtl/amba_vip_if.sv(189) @ 105000: reporter [AMBA_IF] AHB HTRANS invalid |
| AXI4_Lite-UVM-VIP | `axi4_lite_func_test` | checked | PASS | 142 |  |
| AXI_Stream-UVM-VIP | `axi_stream_func_test` | checked | PASS | 122 |  |
| Avalon_MM-UVM-VIP | `avalon_mm_func_test` | checked | PASS | 112 |  |
| Avalon_MM-UVM-VIP | `avalon_mm_ral_test` | checked | PASS | 16896 |  |
| Avalon_ST-UVM-VIP | `avalon_st_func_test` | checked | PASS | 80 |  |
| Bluetooth5-VIP | `bt5_adv_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=3074: UVM_WARNING regression/sva/bluetooth5_compliance_checker.sv(59) @ 343750: reporter [BLUETOOT |
| CAN-VIP | `can_2_0_test` | smoke | FAIL |  | rc=0 UVM_ERROR=5 UVM_FATAL=0 VIP_WARNINGS=82572: UVM_ERROR env/can_env.sv(62) @ 20905000: uvm_test_top.env.sb [SB] mismatch exp{CAN: type=CA |
| CAN_FD-UVM-VIP | `can_fd_func_test` | checked | PASS | 76 |  |
| CAN_XL-UVM-VIP | `can_xl_func_test` | checked | PASS | 92 |  |
| CCIX-VIP | `ccix_random_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=3: UVM_WARNING ccix/ccix_driver.svh(59) @ 3365000: uvm_test_top.env.agt.drv [DRV] credit pool did  |
| CHI-UVM-VIP | `chi_func_test` | checked | PASS | 76 |  |
| CI_CAM-UVM-VIP | `cam_func_test` | checked | PASS | 150 |  |
| CPRI-VIP | `cpri_random_test` | smoke | PASS |  |  |
| CSE-VIP | `mipi_cse_test` | smoke | PASS |  |  |
| CSI2_Sensor-UVM-VIP | `csis_func_test` | checked | FAIL |  | rc=124 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=0 timeout |
| CSI_2-VIP | `mipi_csi2_test` | smoke | FAIL |  | rc=0 UVM_ERROR=18 UVM_FATAL=0 VIP_WARNINGS=31896: UVM_ERROR vip/base/mipi_vip_pkg.sv(576) @ 12506000: uvm_test_top.env.sb [SB] mismatch exp{ |
| CXL-VIP | `mp_cxl_test` | smoke | FAIL |  | rc=1 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=3931: UVM_WARNING vip/base/mp_driver.sv(42) @ 0: uvm_test_top.env.cxl20_agent.drv [CONFIG] Protoco |
| CXS-VIP | `cxs_random_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=3: UVM_WARNING tb/top_tb.sv(84) @ 195000: reporter [DUT] cxs receiver: packet abandoned without cx |
| C_PHY-VIP | `mipi_cphy_test` | smoke | FAIL |  | rc=0 UVM_ERROR=21 UVM_FATAL=0 VIP_WARNINGS=75202: UVM_ERROR vip/base/mipi_vip_pkg.sv(584) @ 12482000: uvm_test_top.env.sb [SB] mismatch exp{ |
| DBI-VIP | `dbi_random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=34 UVM_FATAL=0 VIP_WARNINGS=1: UVM_ERROR env/dbi_env.sv(45) @ 145000: uvm_test_top.env.sb [SB] short WRX not flagged, obs{DBI |
| DDR-VIP | `memory_directed_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=1124: UVM_WARNING env/vip/ddr/ddr_protocol_checker.sv(19) @ 155000: reporter [DDR_CHK] CKE high fo |
| DDR5-VIP | `ddr5_func_test` | checked | ERROR |  | msim: tests/ddr5_tests.sv:76: lower: unsupported rand variable wider than 64 bits |
| DFI-VIP | `dfi_random_test` | smoke | PASS |  |  |
| DPI-VIP | `dpi_random_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=17: UVM_WARNING dpi/dpi_monitor.svh(60) @ 765000: uvm_test_top.env.agt.mon [MON] DE in blanking (l |
| DSI-VIP | `mipi_dsi_test` | smoke | FAIL |  | rc=0 UVM_ERROR=18 UVM_FATAL=0 VIP_WARNINGS=11174: UVM_ERROR vip/base/mipi_vip_pkg.sv(576) @ 12506000: uvm_test_top.env.sb [SB] mismatch exp{ |
| D_PHY-VIP | `mipi_dphy_test` | smoke | FAIL |  | rc=0 UVM_ERROR=18 UVM_FATAL=0 VIP_WARNINGS=25730: UVM_ERROR vip/base/mipi_vip_pkg.sv(576) @ 12506000: uvm_test_top.env.sb [SB] mismatch exp{ |
| DigRF-VIP | `mipi_digrf_test` | smoke | FAIL |  | rc=0 UVM_ERROR=30 UVM_FATAL=0 VIP_WARNINGS=74: UVM_ERROR vip/digrf/digrf_protocol_checker.sv(47) @ 106000: reporter [DIGRF_CHK] DigRF I/Q un |
| DisplayPort2-VIP | `sata_random_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=1541: UVM_WARNING displayport2/displayport2_driver.svh(26) @ 0: uvm_test_top.env.dp2_agt.drv [CONF |
| ETH800G-VIP | `eth800g_func_test` | checked | PASS | 76 |  |
| EtherCAT-VIP | `ethercat_func_test` | checked | PASS | 76 |  |
| Ethernet-VIP | `net_ethernet_100g_test` | smoke | FAIL |  | rc=-9 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=76451: UVM_ERROR vip/base/net_vip_pkg.sv(760) @ 866000: uvm_test_top.env.sb [SCOREBOARD] end-to-e |
| Ethernet_AVB_TSN-VIP | `avb_stream_test` | smoke | FAIL |  | rc=125 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=60827 log flood (>512 MB, killed): UVM_ERROR common/common_pkg.sv(72) @ 852000: uvm_test_top.env |
| FC-VIP | `net_fc_test` | smoke | FAIL |  | rc=-9 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=85837: UVM_WARNING rtl/sva/fc_compliance_checker.sv(56) @ 102000: reporter [FC_L1] Speed valid fa |
| FlexRay-VIP | `random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=510 UVM_FATAL=0 VIP_WARNINGS=17812: UVM_ERROR flexray/sva/flexray_compliance_checker.sv(102) @ 11255000: reporter [SVA_L4] fl |
| GMII-UVM-VIP | `gmii_func_test` | checked | PASS | 76 |  |
| GMII_PHY-UVM-VIP | `gphy_func_test` | checked | PASS | 128 |  |
| GPIO-UVM-VIP | `gpio_func_test` | checked | PASS | 102 |  |
| HBM-VIP | `memory_directed_test` | smoke | ERROR |  | msim: tb_top.sv:124: lower: unsupported rand variable wider than 64 bits |
| HBM3E-VIP | `hbm3e_func_test` | checked | ERROR |  | msim: tests/hbm3e_tests.sv:76: lower: unsupported rand variable wider than 64 bits |
| HBM5-VIP | `hbm5_directed_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=28482: UVM_WARNING hbm5_vip/vip/hbm5/hbm5_protocol_checker.sv(22) @ 105000: reporter [HBM5] Pseudo |
| HDCP-VIP | `hdcp_random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=15 UVM_FATAL=0 VIP_WARNINGS=11: UVM_ERROR env/hdcp_env.sv(66) @ 22455000: uvm_test_top.env.sb [SB] mismatch exp{HDCP aksv=bfa |
| HDMI-VIP | `hdmi_random_test` | smoke | ERROR |  | msim: unordered_map::at |
| HDMI_CEC-UVM-VIP | `cec_func_test` | checked | PASS | 122 |  |
| HDMI_Source-UVM-VIP | `hdmisrc_func_test` | checked | PASS | 104 |  |
| HSI-VIP | `mipi_hsi_test` | smoke | FAIL |  | rc=0 UVM_ERROR=29 UVM_FATAL=0 VIP_WARNINGS=10: UVM_ERROR vip/base/mipi_vip_pkg.sv(608) @ 4726000: uvm_test_top.env.sb [SCOREBOARD] end-to-en |
| HyperBus-UVM-VIP | `hyperbus_func_test` | checked | PASS | 136 |  |
| I2C-VIP | `i2c_random_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=50: UVM_WARNING i2c/i2c_driver.svh(81) @ 935000: uvm_test_top.env.agt.drv [DRV] addr 0x10 NACKed |
| I2S-VIP | `i2s_random_test` | smoke | PASS |  |  |
| I2S_Codec-UVM-VIP | `i2sc_func_test` | checked | FAIL |  | rc=0 UVM_ERROR=2 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR i2sc/i2sc_checker.svh(37) @ 381935000: uvm_test_top.env.chk [CHK] I2SC: burst shorter |
| I3C-VIP | `i3c_random_test` | smoke | ERROR |  | msim: i3c/i3c_sequences.svh:38: lower: unsupported an array or container of the caller in with {...} |
| IO_Link-VIP | `io_link_func_test` | checked | PASS | 84 |  |
| IR_Remote-UVM-VIP | `ir_func_test` | checked | PASS | 124 |  |
| Interlaken-VIP | `intlkn_random_test` | smoke | PASS |  |  |
| JESD204-VIP | `jesd204_random_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=6: UVM_WARNING jesd204/jesd204_monitor.svh(75) @ 655000: uvm_test_top.env.agt.mon [MON] ILAS order |
| JTAG-VIP | `jtag_random_test` | smoke | ERROR |  | msim: regression/vip/jtag/jtag_timing_checker.sv:43: lower: unsupported rand variable wider than 64 bits |
| LIN-VIP | `random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=35 UVM_FATAL=0 VIP_WARNINGS=75336: UVM_ERROR common/common_pkg.sv(63) @ 10335000: uvm_test_top.env.agt.chk [PROTOCOL_CHK] [ER |
| LPDDR-VIP | `memory_directed_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=584: UVM_WARNING lpddr/sva/lpddr_compliance_checker.sv(26) @ 155000: reporter [LPDDR_L1] Address v |
| LPDDR6-VIP | `lpddr6_func_test` | checked | PASS | 112 |  |
| LPI-VIP | `lpi_random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=38 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR env/lpi_env.sv(63) @ 295000: uvm_test_top.env.sb [SB] mismatch exp{LPI request deny= |
| LTI-VIP | `lti_random_test` | smoke | PASS |  |  |
| MACsec-UVM-VIP | `macsec_func_test` | checked | PASS | 114 |  |
| MCTP-UVM-VIP | `mctp_func_test` | checked | PASS | 90 |  |
| MDIO-UVM-VIP | `mdio_func_test` | checked | FAIL |  | rc=0 UVM_ERROR=2 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR env/mdio_env.sv(103) @ 50605000: uvm_test_top.env.sb [SB] 37 driven / 0 observed item |
| MIL_STD_1553-VIP | `mil_std_1553_func_test` | checked | PASS | 76 |  |
| MPEG_TS-UVM-VIP | `tsi_func_test` | checked | PASS | 164 |  |
| M_PHY-VIP | `mipi_mphy_test` | smoke | FAIL |  | rc=0 UVM_ERROR=18 UVM_FATAL=0 VIP_WARNINGS=74311: UVM_ERROR vip/base/mipi_vip_pkg.sv(576) @ 12470000: uvm_test_top.env.sb [SB] mismatch exp{ |
| NC-SI-UVM-VIP | `ncsi_func_test` | checked | PASS | 122 |  |
| NVMe-VIP | `` | smoke | NONE |  | no UVM test |
| OBI-UVM-VIP | `obi_func_test` | checked | PASS | 102 |  |
| OCP-VIP | `ocp_ral_test` | checked | PASS | 1538 |  |
| ONFI-VIP | `onfi_deep_test` | smoke | FAIL |  | rc=0 UVM_ERROR=56 UVM_FATAL=1 VIP_WARNINGS=0: UVM_ERROR env/vip/onfi/onfi_data_integrity_checker.sv(43) @ 150000: reporter [ONFI_DI] X on IO |
| OneWire-UVM-VIP | `onewire_func_test` | checked | FAIL |  | rc=0 UVM_ERROR=47 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR onewire/onewire_checker.svh(37) @ 405000: uvm_test_top.env.chk [CHK] 1-Wire: no pres |
| PCIe-VIP | `mp_pcie_gen5_test` | smoke | FAIL |  | rc=1 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=1350: UVM_WARNING vip/base/mp_driver.sv(42) @ 0: uvm_test_top.env.pcie_gen5_agent.drv [CONFIG] Pro |
| PCIe_IDE-UVM-VIP | `ide_func_test` | checked | PASS | 136 |  |
| PTP-VIP | `ptp_func_test` | checked | PASS | 74 |  |
| PWM-UVM-VIP | `pwm_func_test` | checked | PASS | 78 |  |
| RFFE-VIP | `rffe_random_test` | smoke | PASS |  |  |
| RGMII-UVM-VIP | `rgmii_func_test` | checked | PASS | 74 |  |
| RVFI-UVM-VIP | `rvfi_func_test` | checked | PASS | 124 |  |
| RapidIO-VIP | `rapidio_func_test` | checked | PASS | 74 |  |
| SAS-VIP | `sas_random_test` | smoke | FAIL |  | rc=124 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=0 timeout |
| SATA-VIP | `` | smoke | NONE |  | no UVM test |
| SCCB_Sensor-UVM-VIP | `sccb_func_test` | checked | PASS | 104 |  |
| SD-VIP | `sd_regression_test` | smoke | FAIL |  | rc=-9 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=8: UVM_ERROR env/memory_env_pkg.sv(129) @ 2420000: uvm_test_top.env.sb [MEM_SB] SD mismatch exp{S |
| SDIO-UVM-VIP | `sdio_func_test` | checked | PASS | 76 |  |
| SDMMC_Card-UVM-VIP | `sdm_func_test` | checked | PASS | 176 |  |
| SD_Card-UVM-VIP | `sdc_func_test` | checked | PASS | 162 |  |
| SENT-VIP | `sent_func_test` | checked | PASS | 72 |  |
| SGMII-UVM-VIP | `sgmii_func_test` | checked | PASS | 82 |  |
| SLIMbus-VIP | `slimbus_random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=50 UVM_FATAL=0 VIP_WARNINGS=3: UVM_ERROR env/slimbus_env.sv(54) @ 1540000: uvm_test_top.env.sb [SB] mismatch exp{SLIMbus SB_C |
| SMBus-UVM-VIP | `smbus_func_test` | checked | PASS | 108 |  |
| SPDIF-UVM-VIP | `spdif_func_test` | checked | PASS | 132 |  |
| SPI-VIP | `spi_random_test` | smoke | PASS |  |  |
| SPI_NOR-UVM-VIP | `snor_func_test` | checked | PASS | 132 |  |
| SPMI-VIP | `spmi_random_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=28: UVM_WARNING spmi/spmi_driver.svh(89) @ 1235000: uvm_test_top.env.agt.drv [DRV] sa 0x1 NACKed c |
| SUE-UVM-VIP | `sue_func_test` | checked | PASS | 114 |  |
| SWD-VIP | `swd_random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=32 UVM_FATAL=0 VIP_WARNINGS=40: UVM_ERROR env/swd_env.sv(106) @ 1275000: uvm_test_top.env.sb [SB] mismatch exp{SWD AP RD a=0  |
| Security-VIP | `security_random_test` | smoke | FAIL |  | rc=124 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=0 timeout |
| Smartcard_ISO7816-UVM-VIP | `scd_func_test` | checked | PASS | 132 |  |
| SoundWire-VIP | `soundwire_random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=26 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR soundwire/soundwire_sequences.svh(18) @ 3860000: uvm_test_top.env.agt.sqr@@seq [RAND |
| SpaceWire-UVM-VIP | `spacewire_func_test` | checked | PASS | 102 |  |
| T1S_PLCA-UVM-VIP | `t1s_plca_func_test` | checked | PASS | 84 |  |
| TDM-UVM-VIP | `tdm_func_test` | checked | PASS | 82 |  |
| TileLink-VIP | `tilelink_ral_test` | checked | PASS | 1600 |  |
| ToggleNAND-VIP | `tnand_random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=35 UVM_FATAL=0 VIP_WARNINGS=6: UVM_ERROR env/tnand_env.sv(61) @ 1245000: uvm_test_top.env.sb [SB] mismatch exp{TNAND PROG cmd |
| TypeC-VIP | `typec_random_test` | smoke | FAIL |  | rc=124 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=0 timeout |
| UALink-VIP | `ualink_func_test` | checked | PASS | 106 |  |
| UART-VIP | `uart_random_test` | smoke | FAIL |  | rc=0 UVM_ERROR=1 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR env/uart_env.sv(100) @ 128735000: uvm_test_top.env.sb [SB] missing loopback echo for  |
| UCIe-VIP | `ucie_func_test` | checked | PASS | 72 |  |
| UEC-VIP | `uec_func_test` | checked | PASS | 80 |  |
| UFS-VIP | `ufs_regression_test` | smoke | FAIL |  | rc=0 UVM_ERROR=41 UVM_FATAL=0 VIP_WARNINGS=59: UVM_ERROR ufs/ufs_pkg.sv(495) @ 630000: uvm_test_top.env.ufs_ag.sqr@@q [ufs_query_seq] Rand f |
| USB-VIP | `usb_random_regression_test` | smoke | FAIL |  | rc=0 UVM_ERROR=726 UVM_FATAL=0 VIP_WARNINGS=4058 timeout: UVM_ERROR vip/base/usb_vip_pkg.sv(898) @ 142000: uvm_test_top.env.sb [SCOREBOARD]  |
| USB2-VIP | `usb_eusb2_test` | smoke | FAIL |  | rc=125 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=22371 timeout log flood (>512 MB, killed): UVM_ERROR rtl/usb_vip_if.sv(313) @ 102000: reporter [ |
| USB3-VIP | `usb_random_regression_test` | smoke | FAIL |  | rc=0 UVM_ERROR=726 UVM_FATAL=0 VIP_WARNINGS=19766 timeout: UVM_ERROR vip/base/usb_vip_pkg.sv(898) @ 142000: uvm_test_top.env.sb [SCOREBOARD] |
| USB4-VIP | `usb4_test` | smoke | FAIL |  | rc=124 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=35 timeout: UVM_ERROR rtl/usb_vip_if.sv(313) @ 102000: reporter [USB_IF] Invalid USB device stat |
| USB_PD-VIP | `usb_pd_test` | smoke | FAIL |  | rc=-9 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=7647: UVM_ERROR rtl/usb_vip_if.sv(313) @ 102000: reporter [USB_IF] Invalid USB device state |
| USXGMII-UVM-VIP | `usxgmii_func_test` | checked | PASS | 120 |  |
| UniPro-VIP | `mipi_unipro_test` | smoke | FAIL |  | rc=0 UVM_ERROR=18 UVM_FATAL=0 VIP_WARNINGS=81463: UVM_ERROR vip/base/mipi_vip_pkg.sv(568) @ 12470000: uvm_test_top.env.sb [SB] mismatch exp{ |
| UniPro_Mem-VIP | `memory_coverage_regression_test` | smoke | FAIL |  | rc=0 UVM_ERROR=116 UVM_FATAL=0 VIP_WARNINGS=1440: UVM_ERROR env/memory_env_pkg.sv(86) @ 24610000: uvm_test_top.env.sb [MEM_SB] unipro mismat |
| VbyOne_HS-UVM-VIP | `vbo_func_test` | checked | PASS | 92 |  |
| WTB-UVM-VIP | `wtb_func_test` | checked | PASS | 92 |  |
| Wishbone-UVM-VIP | `wishbone_func_test` | checked | PASS | 136 |  |
| Wishbone-UVM-VIP | `wishbone_ral_test` | checked | PASS | 4224 |  |
| XGMII-UVM-VIP | `xgmii_func_test` | checked | PASS | 76 |  |
| ddr6-VIP | `ddr6_directed_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=27461: UVM_WARNING ddr6_vip/vip/ddr6/ddr6_protocol_checker.sv(22) @ 105000: reporter [DDR6_L1] Ban |
| ddr7-VIP | `ddr7_directed_test` | smoke | WARN |  | rc=0 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=28438: UVM_WARNING ddr7_vip/vip/ddr7/ddr7_protocol_checker.sv(22) @ 105000: reporter [DDR7] Bank g |
| eCPRI-VIP | `ecpri_random_test` | smoke | PASS |  |  |
| eMMC-VIP | `emmc_regression_test` | smoke | FAIL |  | rc=-9 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=15: UVM_ERROR emmc/emmc_pkg.sv(529) @ 2580000: uvm_test_top.env.emmc_ag.sqr@@i [emmc_init_seq] Ra |
| eMMC_Device-UVM-VIP | `emd_func_test` | checked | PASS | 160 |  |
| eSPI-UVM-VIP | `espi_func_test` | checked | PASS | 144 |  |
| eUSB2-VIP | `usb_eusb2_test` | smoke | FAIL |  | rc=125 UVM_ERROR=0 UVM_FATAL=0 VIP_WARNINGS=22250 timeout log flood (>512 MB, killed): UVM_ERROR rtl/usb_vip_if.sv(313) @ 102000: reporter [ |
| gddr5-VIP | `gddr5_write_test` | smoke | FAIL |  | rc=0 UVM_ERROR=32 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR gddr5_vip/vip/gddr5/gddr5_compliance_checker.sv(123) @ 135000: reporter [SVA_L4] gdd |
| gddr6-VIP | `gddr6_write_test` | smoke | FAIL |  | rc=0 UVM_ERROR=32 UVM_FATAL=0 VIP_WARNINGS=494: UVM_ERROR gddr6_vip/vip/gddr6/gddr6_compliance_checker.sv(129) @ 135000: reporter [SVA_L4] g |
| gddr7-VIP | `gddr7_write_test` | smoke | FAIL |  | rc=0 UVM_ERROR=32 UVM_FATAL=0 VIP_WARNINGS=430: UVM_ERROR gddr7_vip/vip/gddr7/gddr7_compliance_checker.sv(135) @ 135000: reporter [SVA_L4] g |
| hbm2-VIP | `hbm2_write_test` | smoke | FAIL |  | rc=0 UVM_ERROR=32 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR hbm2_vip/vip/hbm2/hbm2_compliance_checker.sv(123) @ 135000: reporter [SVA_L4] hbm2:  |
| hbm3-VIP | `hbm3_write_test` | smoke | FAIL |  | rc=0 UVM_ERROR=32 UVM_FATAL=0 VIP_WARNINGS=0: UVM_ERROR hbm3_vip/vip/hbm3/hbm3_compliance_checker.sv(129) @ 135000: reporter [SVA_L4] hbm3:  |
| hbm4-VIP | `hbm4_write_test` | smoke | FAIL |  | rc=0 UVM_ERROR=32 UVM_FATAL=0 VIP_WARNINGS=494: UVM_ERROR hbm4_vip/vip/hbm4/hbm4_compliance_checker.sv(135) @ 135000: reporter [SVA_L4] hbm4 |
| lpddr5-VIP | `lpddr5_write_test` | smoke | FAIL |  | rc=0 UVM_ERROR=32 UVM_FATAL=0 VIP_WARNINGS=558: UVM_ERROR lpddr5_vip/vip/lpddr5/lpddr5_compliance_checker.sv(123) @ 135000: reporter [SVA_L4 |
| lpddr5x-VIP | `lpddr5x_rw_test` | smoke | FAIL |  | rc=0 UVM_ERROR=251 UVM_FATAL=0 VIP_WARNINGS=9378: UVM_ERROR lpddr5x_vip/sva/lpddr5x_compliance_checker.sv(126) @ 215000: reporter [SVA_L3] l |
| lpddr7-VIP | `lpddr7_rw_test` | smoke | FAIL |  | rc=0 UVM_ERROR=251 UVM_FATAL=0 VIP_WARNINGS=14556: UVM_ERROR lpddr7_vip/sva/lpddr7_compliance_checker.sv(126) @ 215000: reporter [SVA_L3] lp |
| xSPI-UVM-VIP | `xspi_func_test` | checked | PASS | 146 |  |
