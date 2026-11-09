# UVM on minisim

minisim runs testbenches written with the Universal Verification Methodology
(IEEE 1800.2-2020), using the Accellera reference library
[uvm-core](https://github.com/accellera-official/uvm-core) as it is.

## Running

```sh
git clone --depth 1 --branch 2020.3.1 https://github.com/accellera-official/uvm-core
export UVM_HOME=$PWD/uvm-core
msim -uvm my_tb.sv -top top +UVM_TESTNAME=my_test
```

`-uvm` adds `+incdir+$UVM_HOME/src` and `uvm_pkg.sv` ahead of your files and
builds UVM's C code into a DPI library (`-uvmhome <dir>` names the checkout
instead of `$UVM_HOME`). That C code is uvm-core's own `src/dpi` sources,
compiled with minisim's `uvm/uvm_dpi_minisim.cc`, which supplies the HDL
backdoor through plain VPI in place of uvm-core's per-vendor files. With it:

- regular expressions work in `uvm_config_db` and factory paths, and in
  `uvm_re_match`;
- the whole command line is read: `+uvm_set_config_int`,
  `+uvm_set_config_string`, `+uvm_set_verbosity`, `+UVM_VERBOSITY`,
  `+uvm_set_type_override`, `uvm_cmdline_processor::get_args` and the
  rest;
- HDL backdoor access works: `uvm_hdl_check_path`, `uvm_hdl_read`,
  `uvm_hdl_deposit`, `uvm_hdl_force`, `uvm_hdl_release` on variables and
  nets by hierarchical name, and so register model backdoor reads and
  writes (`UVM_BACKDOOR`) through `add_hdl_path` / `configure(..., "path")`.

The C code calls VPI from context imports, so the design runs on one thread.

`-uvmnodpi` builds the library without its C code (`+define+UVM_NO_DPI`).
In that mode UVM itself turns off the three things above: patterns are globs
only (`*`, `?`), only `+UVM_TESTNAME` is read from the command line, and the
register layer has no backdoor. It is the same as

```sh
msim +incdir+$UVM_HOME/src $UVM_HOME/src/uvm_pkg.sv my_tb.sv -top top +define+UVM_NO_DPI
```

## What is covered

`tests/uvm` holds the UVM regression, run in CI against uvm-core 2020.3.1
(configure with `-DUVM_HOME=<uvm-core checkout>` to run it locally). Each
test runs with `msim -uvmhome`, so with UVM's C code, unless it says
`//! nodpi`:

| Test | What it exercises |
|---|---|
| `hello.sv` | `run_test`, the factory, a task phase, objections, `` `uvm_info `` |
| `agent.sv` | sequence, sequencer, driver, monitor, scoreboard around a DUT; a virtual interface through `uvm_config_db`; analysis port and imp |
| `dpi.sv` | UVM's C code: `uvm_re_match`, `uvm_cmdline_processor`, `+uvm_set_config_int`, `uvm_hdl_*` read / deposit / force / release on variables and nets, a register read and written through its backdoor |
| `hello_nodpi.sv` | `hello.sv` with the library built without its C code |
| `features.sv` | field macros (print, clone, compare, sprint, pack), a factory type override, a TLM FIFO between put and get ports, a report catcher, `print_topology`, `randomize() with`, `+UVM_TESTNAME` |

## Not yet

- `uvm_hdl_force_time` and the backdoor on VHDL objects (minisim simulates
  SystemVerilog only); `uvm_hdl_*` on a part select path (`top.r[3:0]`).
- Rand associative arrays and rand arrays of
  arrays.

## Debugging

A run-time error such as a null object access names the file and line of
the statement that was running. `MSIM_CALLTRACE=1` prints every call of a
class method or automatic subroutine, with its object and call site.
