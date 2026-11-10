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
`--csv` saves a run and `--expect <csv>` compares with a saved one, failing
only when a test that passed there no longer does. Logs are in
`mvip_work/logs/`.
