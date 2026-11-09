# UVM on minisim

minisim runs testbenches written with the Universal Verification Methodology
(IEEE 1800.2-2020), using the Accellera reference library
[uvm-core](https://github.com/accellera-official/uvm-core) as it is.

## Running

```sh
git clone --depth 1 --branch 2020.3.1 https://github.com/accellera-official/uvm-core
U=uvm-core/src
msim +incdir+$U $U/uvm_pkg.sv my_tb.sv -top top +define+UVM_NO_DPI +UVM_TESTNAME=my_test
```

`+define+UVM_NO_DPI` builds the library without its C code. In that mode
UVM itself turns off three things: regular expressions in `uvm_config_db` and
factory paths (glob patterns such as `*` and `?` still work), reading the
whole command line (so `+UVM_TESTNAME` works but `+uvm_set_config_*` and
`+UVM_VERBOSITY` do not), and HDL backdoor access in the register layer.

## What is covered

`tests/uvm` holds the UVM regression, run in CI against uvm-core 2020.3.1
(configure with `-DUVM_HOME=<uvm-core checkout>` to run it locally):

| Test | What it exercises |
|---|---|
| `hello.sv` | `run_test`, the factory, a task phase, objections, `` `uvm_info `` |
| `agent.sv` | sequence, sequencer, driver, monitor, scoreboard around a DUT; a virtual interface through `uvm_config_db`; analysis port and imp |
| `features.sv` | field macros (print, clone, compare, sprint, pack), a factory type override, a TLM FIFO between put and get ports, a report catcher, `print_topology`, `randomize() with`, `+UVM_TESTNAME` |

## Not yet

- The UVM DPI code (`src/dpi/uvm_dpi.cc`): regular expressions, the full
  command line, and HDL backdoor access need it, and it uses VPI.
- Function calls in constraints, rand associative arrays and rand arrays of
  arrays.

## Debugging

A run-time error such as a null object access names the file and line of
the statement that was running. `MSIM_CALLTRACE=1` prints every call of a
class method or automatic subroutine, with its object and call site.
