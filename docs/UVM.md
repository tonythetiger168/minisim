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
  `uvm_hdl_deposit`, `uvm_hdl_force`, `uvm_hdl_release`,
  `uvm_hdl_force_time` on variables and nets by hierarchical name, and on a
  bit or part select of one (`top.u.r[3:0]`), and so register model backdoor reads and
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
| `dpi.sv` | UVM's C code: `uvm_re_match`, `uvm_cmdline_processor`, `+uvm_set_config_int`, `uvm_hdl_*` read / deposit / force / release on variables, nets and part selects, `uvm_hdl_force_time`, a register read and written through its backdoor |
| `hello_nodpi.sv` | `hello.sv` with the library built without its C code |
| `features.sv` | field macros (print, clone, compare, sprint, pack), a factory type override, a TLM FIFO between put and get ports, a report catcher, `print_topology`, `randomize() with`, `+UVM_TESTNAME` |
| `base_extras.sv` | `uvm_time`, `uvm_heartbeat` (a missed beat is reported), `uvm_sequence_library` (RANDC and RAND selection, add and remove), the report macros with counts, `uvm_callback_iter`, `print_config`, package scope functions, `uvm_run_test_callback`, `uvm_root` find / find_all |
| `reg_extras.sv` | `uvm_reg_file` with its HDL path, register randomization, `uvm_reg_indirect_data`, `uvm_reg_fifo`, `uvm_vreg` / `uvm_vreg_field` (static, allocated and implemented), `uvm_mem_mam` and regions, the `uvm_reg_block` lookups |
| `reg_sequences.sv` | `uvm_reg_access_seq`, the shared access sequences, `uvm_mem_access_seq`, `uvm_reg_mem_hdl_paths_seq`, `uvm_reg_mem_built_in_seq`: each runs clean on a good DUT and reports an error on a broken one |

## Not yet

- The backdoor on VHDL objects (minisim simulates SystemVerilog only).
- Rand arrays of arrays (`rand int q[$][]` with `foreach (q[i, j])`
  constraints); uvm-core itself does not use them.

## Debugging

A run-time error such as a null object access names the file and line of
the statement that was running. `MSIM_CALLTRACE=1` prints every call of a
class method or automatic subroutine, with its object and call site.
