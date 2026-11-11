# mpss: portable stimulus

`mpss` reads a test model written in the Accellera Portable Test and Stimulus
Standard language (PSS), solves scenarios from it, and writes each scenario
twice: as a UVM sequence for simulation and as a C function for the
processor. Both come from the same solved scenario, with the same field
values, so a test that fails on silicon can be replayed in simulation.

```sh
mpss dma.pss -n 20 -seed 1 -print -sv dma_tests_pkg.sv -c dma_tests.c
```

A model says what the design can do (actions), what each action needs and
gives (buffers, streams, resources), and which combinations are legal
(constraints). A test intent is a short activity; mpss fills in the rest: it
picks field values, picks which resource instance each action takes, and
adds the actions that are needed to make the scenario legal, such as the one
that initializes a memory region before a copy reads it.

## Options

| Option | Meaning |
|---|---|
| `-top <component>` | Root component. Default `pss_top`. |
| `-root <action>` | Root action. Default `<top>::entry`, or the only compound action of the root component. |
| `-n <count>` | Number of scenarios. Default 1. |
| `-seed <n>` | Random seed. Default 1. |
| `-name <base>` | Base name of the generated code. Default: the root action's name. |
| `-sv <file>` | Write package `<base>_pkg` with one `uvm_sequence` class per scenario. |
| `-c <file>` | Write one C function per scenario. |
| `-json <file>` | Write the solved scenarios as JSON (the action tree, every field, every object). |
| `-print` | Print the solved scenarios. |
| `-check` | Read and elaborate the model, then stop. |
| `-threads <n>` | Worker threads (also `-j`). Default: every logical CPU, or `$MINISIM_THREADS`. |
| `-q` | No summary line. |

Scenario *i* depends only on the model, `-seed` and *i*. Scenarios are
solved in parallel and written in order, so the output is byte for byte the
same for every `-threads` value, and scenario 7 of `-n 10` is scenario 7 of
`-n 1000`.

## An example

`examples/pss/dma.pss` models a DMA controller with four channels:

```pss
buffer data_buff_s {
  rand bit[32] in [0x1000..0x1fff] addr;
  rand bit[16] in [4..256] size;
  constraint addr % 4 == 0;
  constraint addr + size <= 0x2000;
}
resource channel_s { }

component dma_c {
  pool [4] channel_s chan_p;
  bind chan_p *;
  action mem2mem {
    input data_buff_s src;
    output data_buff_s dst;
    lock channel_s chan;
    constraint dst.size == src.size;
    constraint dst.addr >= src.addr + src.size || src.addr >= dst.addr + dst.size;
    exec body {
      dma_copy(chan.instance_id, src.addr, dst.addr, src.size);
    }
  }
}
```

The entry activity asks for two copies in parallel, then more copies and
checks. `mpss -print` shows what was solved:

```
scenario 0 (seed 1)
  sequence pss_top::entry @pss_top: rounds=1
    parallel
      sequence
        pss_top::init_mem @pss_top (inferred): seed=165 dst=#6
        dma_c::mem2mem @pss_top.dma0: chan=chan_p[3] dst=#0 src=#6
      sequence
        pss_top::init_mem @pss_top (inferred): seed=235 dst=#7
        dma_c::mem2mem @pss_top.dma0: chan=chan_p[2] dst=#2 src=#7
    dma_c::mem2mem @pss_top.dma0: chan=chan_p[3] dst=#4 src=#2
    pss_top::check_mem @pss_top: src=#0
```

The two parallel copies took different channels, the regions of each copy do
not overlap, and each copy got an `init_mem` in front of it because nothing
in the activity produced its source buffer.

## The C output

One function per scenario, plus a table of them:

```c
extern void dma_copy(uint8_t chan, uint32_t src, uint32_t dst, uint16_t size);

void entry_0(void)
{
  /* inferred5: pss_top::init_mem @pss_top (inferred): seed=165 dst=#6 */
  {
    mem_fill(0x145cu, 56, 165);
  }
  /* entry.a: dma_c::mem2mem @pss_top.dma0: chan=chan_p[3] dst=#0 src=#6 */
  {
    dma_copy(3, 0x145cu, 0x1424u, 56);
  }
  ...
}
void (*const entry_tests[])(void) = {entry_0, entry_1};
const unsigned entry_count = 2;
```

- A function the model imports (`import target function ...;` or a
  `function` with no body) is an `extern` the test platform provides.
- A function the model defines is generated as a `static` C function.
- `exec header C = """..."""` and `exec declaration C = """..."""` text goes
  to the top of the file.
- `-DMPSS_MAIN` adds a `main` that runs every scenario.
- The branches of a `parallel` block run one after the other. Running them
  on several cores is not done yet.

## The UVM output

`-sv` writes a package with:

- `<base>_api`, a virtual class with one pure virtual task per imported
  function. A function that returns a value becomes a task with an output
  argument `mpss_ret`, because on a bus it takes time. The testbench extends
  the class and drives its agents in each task.
- `<base>_base_seq`, a `uvm_sequence` with the `api` handle and the model's
  own functions.
- `<base>_<i>_seq`, one class per scenario, registered with the factory. Its
  `body()` is the scenario; a `parallel` block is a `fork ... join`.

```systemverilog
class dma_api extends entry_api;
  virtual task dma_copy(input bit [7:0] chan, input bit [31:0] src,
                        input bit [31:0] dst, input bit [15:0] size);
    // program the DMA through the register agent, wait for the interrupt
  endtask
  ...
endclass

uvm_config_db#(entry_api)::set(null, "*", "mpss_api", api);   // or seq.api = api
entry_0_seq seq = entry_0_seq::type_id::create("seq");
seq.start(null);
```

```sh
msim -uvm dma_tests_pkg.sv my_tb.sv -top top +UVM_TESTNAME=my_test
```

`tests/pss/dma_tb.sv` is a complete small testbench. Each action reports
itself with `` `uvm_info("MPSS", ..., UVM_HIGH)``.

`print("...", args)` in an exec block is `printf` in C and `$write` in
SystemVerilog, with the number formats adjusted so both print the same text.
The test suite runs the same scenarios as C and under `msim -uvm` and
compares the output.

## What is supported

| Area | Supported |
|---|---|
| Declarations | `package`, `import`, `component` (fields, arrays, nested instances, `exec init` / `init_up` / `init_down`), `action`, `abstract action`, `struct`, `buffer`, `stream`, `resource`, `enum`, `typedef`, package `const`, inheritance (`: base`), `extend` |
| Data types | `bit[N]`, `bit[hi:lo]`, `int`, `int[N]` (to 64 bits), `bool`, `string` (constant), enums, structs, fixed-size arrays, `array<T, N>`, `in [..]` domains |
| Flow objects | `input` / `output` of buffers and streams, `pool`, `bind` (every pool serves the subtree of its component), activity `bind a.out b.in;` |
| Resources | `lock`, `share`, `pool [N]`, `instance_id` |
| Constraints | expressions with `+ - * / % ** << >> & \| ^ ~ ! && \|\| == != < <= > >= ?:` and `in [..]`, implication `->`, `if` / `else`, `foreach` (with iterator and index), `unique`, named and unnamed blocks, constraints across actions through handles (`a.x + b.x == 3`), inline `with` |
| Activities | action handles, `do type`, `sequence`, `parallel`, `schedule`, `select` with weights and guards, `repeat` (count and index), `replicate`, `if` / `else`, `constraint`, labels |
| Exec blocks | `exec body` with procedural code (variables, assignments, calls, `if`, `repeat`, `while`, `break`, `continue`) and with target templates (`exec body C = """...{{field}}..."""`, `SV` likewise), `header`, `declaration` |
| Functions | `function` with a body, `import [target] [C] function` prototypes, `print` |

Not supported yet (mpss reports the line): `state` objects, address space
claims (`addr_reg_pkg`), registers, `dynamic` and `default` constraints,
`forall`, `foreach` / `match` in activities and exec blocks, lists, maps and
sets, coverage (`covergroup`), template types, `pre_solve` / `post_solve`
exec blocks (they are read and not run), arrays of flow object or resource
references, and multi-core C output.

Arithmetic in constraints is on plain integers (no wrap-around at a field's
width), and division rounds toward zero.

## How scenarios are solved

1. The root action's activity is expanded. `select`, `repeat` counts and
   `if` conditions are decided here; one that reads random fields solves
   that action's own constraints first and fixes those fields.
2. Every input is bound to an output: a buffer to an action that finished
   earlier, a stream to an action running in parallel. If the activity has
   no such action, one that produces the object is inferred and added (and
   so on for what that action needs, up to 16 deep).
3. The constraints are collected: those of every action, struct and flow
   object, the inline ones, and for every two resource claims that overlap
   in time with at least one `lock`, that they take different instances.
4. The solver groups the variables that share constraints and solves each
   group by itself. A variable's value is found by halving its range, a
   random half first; each value chosen narrows the ranges of the variables
   it shares constraints with, and a range that empties undoes the choice.
   Whether a constraint can still hold over a range comes from interval
   arithmetic. The interval rules are shared with `msim`'s `randomize()`
   solver (`src/interval.hpp`).
5. Every constraint is evaluated again on the final values.

A scenario that cannot be solved this way starts again with other random
choices (64 times), and then mpss stops with the reason.
