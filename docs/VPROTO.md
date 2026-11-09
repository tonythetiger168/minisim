# Virtual prototypes in minisim

A virtual prototype is the part of a system that is a program rather than
RTL: a memory, a peripheral, a block nobody has written yet, or a whole
subsystem behind a SystemC/TLM socket. The design drives it over AXI4-Lite
and the model answers in C, so software and RTL meet before the RTL is
finished.

minisim's layer for that is **mvp**: one SystemVerilog file for the design
side, one C header for the model side, and a runtime between them that keeps
the memory map, the interrupt lines, and the models' notion of time. It is
built on DPI-C (clause 35), so a model is ordinary C or C++ with no knowledge
of minisim's internals.

```sh
msim -mvp soc_tb.sv my_model.c -top soc_tb
```

`-mvp` adds `mvp.sv` and the runtime; your model files go on the same command
line. Without `-mvp`, name them yourself:
`msim <prefix>/share/minisim/mvp/sv/mvp.sv ... <prefix>/share/minisim/mvp/mvp.c`.

## The design side

`mvp.sv` holds three modules, each with `timeunit 1ps`, so the time the models
see is in picoseconds whatever timescale the design uses.

| Module | What it is |
| --- | --- |
| `mvp_axil_slave #(ADDR_W, DATA_W, ADDR_OFFSET, VERBOSE)` | An AXI4-Lite slave in front of the memory map in C. `DATA_W` is 32 or 64. One access at a time: the C call happens in the cycle the address is accepted, the response goes out the cycle after. `ADDR_OFFSET` is added before the address reaches C, for a slave behind an interconnect that strips the high bits. |
| `mvp_irq_slave #(N)` | The interrupt lines the models drive with `mvp_set_irq`, sampled every clock. Level sensitive. |
| `mvp_clock #(VERBOSE)` | The models' time and the callbacks they armed. One per design; a slave ticks too, so this is for the timers of a design whose slaves sit idle. `VERBOSE` prints the memory map at time 0. |

An access to an address no model holds, or one a model rejects, comes back as
`SLVERR` (`2'b10`); the simulation keeps running, which is what you want when
the point of the run is to find that access.

## The model side

```c
#include "mvp.h"

static uint8_t regs[16];

static int rd(void* ctx, uint64_t addr, uint32_t bytes, uint64_t* data) {
    *data = regs[addr];
    return MVP_OK;
}
static int wr(void* ctx, uint64_t addr, uint32_t bytes, uint64_t data, uint8_t strb) {
    regs[addr] = (uint8_t)data;
    if (addr == 0) mvp_after_ps(1000000, done, NULL);   /* 1 us later */
    return MVP_OK;
}

MVP_MODEL(my_model_init) {
    mvp_device dev = {0};
    dev.name = "mydev"; dev.base = 0x2000; dev.size = sizeof regs;
    dev.read = rd; dev.write = wr;
    mvp_add_device(&dev);
}
```

Addresses reaching a device are relative to its base, and an access never
crosses the device's end. `read` and `write` run inside the design's clock
process, so they must return without blocking: to take time, arm
`mvp_after_ps` and keep the register side ready, the way real hardware does.

| Function | What it does |
| --- | --- |
| `mvp_add_device` | Put a device in the map. Overlapping devices are refused, with both ranges named. |
| `MVP_MODEL(name) { … }` | A registration function that runs when the library loads, so several model files can live in one library. A single model may define `mvp_model_init` instead. |
| `mvp_time_ps` | The design's time at the last clock edge. |
| `mvp_after_ps` | Run a callback later. The design's clock is the resolution. |
| `mvp_set_irq`, `mvp_get_irq` | Drive and read a line of `mvp_irq_slave`. |
| `mvp_bus_read`, `mvp_bus_write` | An access from a model into the map, for a model that is a bus master (a DMA engine, a CPU model fetching instructions). It reaches the other models, not the design. |
| `mvp_print_map` | Print the map. |

## The bundled models

In `<prefix>/share/minisim/mvp/models` (in the source tree, `mvp/models`).
Both are examples to copy as much as parts to use.

**`ram.c`** — a memory. `MVP_RAM_BASE` (default 0), `MVP_RAM_SIZE` (default
64 KiB), `MVP_RAM_INIT` (a file loaded at offset 0, for a boot image).

**`uart.c`** — a 16550-style UART: write `THR` and the byte appears on stdout,
and the transmit-empty interrupt comes back one character time later, so a
driver's wait loop is exercised rather than skipped. `MVP_UART_BASE` (default
0x10000), `MVP_UART_BAUD` (default 115200; 0 makes it instant), `MVP_UART_IRQ`
(the line, default 0). Registers: `0x00` THR / RBR, `0x04` LSR (THRE, BUSY,
RXRDY), `0x08` IER, `0x0c` IIR.

`tests/vproto/soc_tb.sv` drives both over AXI4-Lite and is the shortest
complete example: a write and a read, byte strobes, a decode error, and the
UART's character time and interrupt.

## SystemC and TLM-2.0

`mvp_tlm.h` puts a TLM-2.0 target behind the same bridge, so a subsystem that
already exists as SystemC answers the design unchanged:

```c++
#include <systemc>
#include "mvp_tlm.h"

my_tlm_device dev("dev");
static mvp::tlm_bridge bridge("ram", 0x0, 0x10000);

MVP_MODEL(init) {
    bridge.socket.bind(dev.socket);
    bridge.add();
}
```

```sh
msim -mvp soc_tb.sv model.cpp -top soc_tb \
     -CFLAGS "-I$SYSTEMC_HOME/include -std=c++17" \
     -LDFLAGS "-L$SYSTEMC_HOME/lib -lsystemc"
```

The design's clock drives SystemC, not the other way round: each access runs
`sc_start` for the time that has passed since the last one, plus
`b_transport`'s own delay, so a TLM model that waits sees time move while the
design runs. A model that must also run between accesses arms `mvp_after_ps`
and calls `mvp::tlm_advance()` from the callback. minisim does not ship
SystemC and the test suite does not use it; nothing else in mvp needs it.

## Attaching an instruction-set simulator

A CPU model is a model like any other, in the other direction: it uses
`mvp_bus_read` / `mvp_bus_write` to fetch and to access its peripherals, and
`mvp_after_ps` to spend time per instruction. For QEMU, the same shape works
over its own transport: run QEMU as a process, speak to it from a model that
forwards `mvp_bus_*` and the interrupt lines across a socket. mvp gives the
design-side half of that; the QEMU side is not in the tree yet.

## Limits

- One access at a time per `mvp_axil_slave`: no write and read overlap, and no
  outstanding-transaction pipelining. For throughput work drive the real bus
  RTL instead.
- AXI4-Lite only (no bursts, no IDs, no QoS), `DATA_W` 32 or 64.
- 32 devices, 64 pending callbacks, 32 interrupt lines.
- A design that uses DPI runs in the interpreter; `-compile` and `-threads`
  do not apply.
