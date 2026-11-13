# mvplat: virtual platforms

`mvplat` runs the software of a system before its hardware exists. A
platform is a set of RISC-V harts and a memory map of peripheral models; the
software is the same ELF file the real board will boot. It is the
counterpart of Synopsys Virtualizer, Cadence Helium and Siemens Vista.

```sh
mvplat prog.elf                                  # the built-in board
mvplat -platform soc.json prog.elf               # a board described in JSON
mvplat -platform soc.json prog.elf -harts 8      # eight harts, on every core
mvplat prog.elf -trace commits.log               # what ran, in spike's format
mvplat -kernel Image -initrd root.cpio -append "console=ttyS0"   # boot Linux under OpenSBI
```

The exit code is the program's own (the test finisher or HTIF `tohost`), so a
software test is a CI job like any other.

**Free and licensed parts.** The engine described here, which includes the
harts, time, several harts on several threads, the bundled devices, the
platform file and the commit log, is part of the free edition
(Apache-2.0). A licensed extension library adds the
following:

- the gdb server (`-gdb`)
- checkpoints (`-save`, `-restore`)
- the profile (`-profile`)
- `mvp` model libraries on the bus (C, C++ and SystemC/TLM-2.0 models, the
  same files `msim -mvp` takes)
- RTL windows simulated by `msim` in lock step, with DMA from the RTL into
  the platform

mvplat loads the extension the first time a run asks for one of these, from
`$MVPLAT_EXT` or from `<prefix>/lib/minisim/libmvplat_ext.so`. The extension
runs on the same engine, not a copy of it: it builds against the
headers installed in `<prefix>/include/minisim/vp` (`vp/ext.hpp` is the
interface). Without the extension, such a run stops with a message that
names what is missing. Sections marked *(extension)* describe these parts.

## What it simulates

**Harts.** RV32GC or RV64GC (IMAFDC with Zicsr and Zifencei), in machine,
supervisor and user mode. The program's ELF class picks the width: a 64-bit
ELF file runs on RV64 harts (`misa.MXL` 2, `UXL` and `SXL` fixed at 64), with
the W instructions, LD/SD/LWU, 64-bit atomics (`.D`) and RV64C. Single and
double floating point (F, D) follow IEEE 754 with every rounding mode, the
exception flags in `fflags`, NaN boxing and canonical NaNs, and `mstatus.FS`
(off until software turns it on; dirty after a write, with `SD`). The CSRs:
`mstatus`, `mtvec` (direct and vectored), `mepc`, `mcause`, `mtval`, `mie`,
`mip`, `mscratch`, `mcounteren`, `mcountinhibit`, the cycle, time and
instret counters, `mhartid`, physical memory protection (16 entries, 4-byte grain: OFF, TOR,
NA4 and NAPOT, locked entries, MPRV), and the Sdtrig CSRs reporting no
triggers. One instruction per cycle; WFI sleeps
until an interrupt is pending. Misaligned loads and stores are done, not
trapped.

**Supervisor mode and the MMU.** `medeleg` and `mideleg` hand exceptions
and interrupts to supervisor mode; `sstatus`, `sie` and `sip` are the
supervisor's views of the machine registers, with `stvec`, `sscratch`,
`sepc`, `scause`, `stval`, `scounteren` and `satp`. SRET, SFENCE.VMA and the
`mstatus` bits SUM, MXR, TVM, TW and TSR work as the privileged
specification says. `satp` selects Sv32 paging on RV32 (two levels, 4 MiB
megapages) and Sv39 on RV64 (three levels, 2 MiB and 1 GiB pages; a virtual
address not sign-extended from bit 38 faults), with a 256-entry TLB per hart
and page faults (causes 12, 13 and 15). No ASIDs (`satp.ASID` reads 0), no
Svnapot or Svpbmt (those PTE bits fault). Accessed and dirty bits follow Svade: a page whose A bit is clear,
or a store to a page whose D bit is clear, faults and the supervisor sets
them, as spike and QEMU do by default. The page walk is checked by PMP.
The PLIC has two contexts per hart, as QEMU's `virt` does: context 2*h*
interrupts hart *h* in machine mode (MEIP) and 2*h*+1 in supervisor mode
(SEIP).

The RISC-V architecture tests all pass, 430 programs: ui, um, ua, uc, uf and
ud for RV32 and RV64, both bare (`p`) and under the riscv-tests supervisor with
paging on (`v`, Sv32 and Sv39), and mi and si for both. `tests/mvplat_isa.sh`
runs them.

**Time.** Each hart runs a quantum of cycles (`-quantum`, default 1000) on
its own and then the platform synchronises: devices see time move and
interrupts are re-evaluated. This is SystemC TLM-2.0's temporal decoupling.
A quantum ends early at the next timer compare and at the next callback an
mvp model has armed, so timer interrupts and model delays arrive on time
whatever the quantum.

**Several harts, several threads.** Harts run in parallel, one per thread
(`-threads N`, default every CPU). Each hart's stores go to its own write
buffer during a round and are written back in hart order at its end;
accesses with side effects (peripheral registers, atomics, LR/SC, HTIF) wait
for the ordered phase of the round and happen in hart order there. The run
therefore does not depend on the thread count or on the operating system's
scheduler: the console, the commit log and the profile are the same, byte
for byte, on 1 thread and on 64. The test suite checks exactly that.

## The platform file

JSON, with `//` and `/* */` comments. Numbers may be written as strings, in
hex, with `K`, `M`, `G` suffixes for sizes. Paths are relative to the file.

```json
{
  "name": "my-soc",
  "harts": 2,
  "mhz": 100,                // hart clock (default 100)
  "mtime_hz": 10000000,      // CLINT mtime rate (default 10 MHz)
  "quantum": 1000,           // cycles between synchronisations
  "reset": "0x80000000",     // the pc before an ELF entry point overrides it
  "devices": [
    {"type": "finisher",  "base": "0x00100000"},
    {"type": "clint",     "base": "0x02000000"},
    {"type": "plic",      "base": "0x0c000000", "sources": 32},
    {"type": "uart16550", "name": "uart0", "base": "0x10000000", "irq": 10, "input": "hi\n"},
    {"type": "rom",       "name": "boot", "base": "0x00001000", "size": "4K", "file": "boot.bin"},
    {"type": "ram",       "name": "dram", "base": "0x80000000", "size": "128M"},
    {"type": "mvp", "name": "models", "irq": 20,
     "sources": ["dma.c", "codec.cpp"], "cflags": "-DFAST",
     "env": {"DMA_BASE": "0x40000000"}},
    {"type": "rtl", "name": "acc", "base": "0x40100000", "size": "0x1000", "irq": 30,
     "msim": ["acc.sv", "acc_tb.sv", "-top", "acc_tb"]}
  ]
}
```

Without `-platform`, the built-in `virt32` board is used: the finisher,
CLINT, PLIC and UART above, and 128 MiB of RAM at `0x80000000` (`-map` prints
it). It is the address map most RISC-V bare-metal software already targets.

| Device | Registers | Notes |
| --- | --- | --- |
| `ram`, `rom` | plain memory | `file` (and `offset`) preloads it; a ROM refuses writes with an access fault |
| `uart16550` | 16550 at `base + (index << shift)` | transmit goes to stdout; `input` (or `-input` for the first UART) is what it receives; interrupt on receive data and on THR empty |
| `clint` | `msip` at `4*h`, `mtimecmp` at `0x4000 + 8*h`, `mtime` at `0xbff8` | `mtime` follows the asking hart's cycle count |
| `plic` | priorities, pending, per-hart enables, threshold, claim/complete | one M-mode context per hart; level-triggered sources |
| `finisher` | `0x5555` passes, `0x3333 \| code << 16` fails with `code` | the SiFive test device |
| `virtio-blk` | virtio over MMIO, version 2 (VIRTIO 1.2, 4.2), one queue | a disk: `file` is a raw image, `irq` its PLIC source; writes stay in the run unless `"write": true`, `"readonly": true` refuses them. See "Disks" |
| `virtio-net` | virtio over MMIO, version 2, a receive and a transmit queue | an Ethernet interface: `mac`, `irq`, `backend` (`user` or `dgram` with `local` and `remote` socket paths), `pcap`. See "Networks" |
| `virtio-console` | virtio over MMIO, version 2, one port | a console (Linux's `hvc0`): output to stdout, `input` typed once the guest has printed and been quiet for 100 ms of simulated time |
| `mvp` | the library's own map (`mvp_add_device`) | see below |
| `rtl` | whatever the RTL decodes | see below |

A program with a `tohost` symbol (the RISC-V tests, proxy kernels) can also
end the run through HTIF: `(code << 1) | 1` written to `tohost`.

## mvp models on the platform *(extension)*

An `mvp` device is a library of models written against `mvp.h`: exactly the
files `msim -mvp` takes. `sources` builds it (with `mvp/mvp.c`, the shared
runtime); `lib` names one already built. The models register their own
devices and addresses; `env` sets the environment variables they read
before they do. Line *n* of the library's interrupts (`mvp_set_irq`) is PLIC
source `irq + n`. A model's `mvp_after_ps` callbacks run at their time, on
the platform's clock, so a UART model's character time is the same here as
in RTL simulation.

So one model serves twice: under `msim -mvp` the RTL CPU drives it over
AXI4-Lite; under `mvplat` the instruction-set simulator does. The SystemC
adapter in `mvp_tlm.h` comes along, which puts any TLM-2.0 target on the
platform's bus (`cflags` / `env` carry the SystemC paths, as in VPROTO.md).

## RTL in the platform *(extension)*

An `rtl` device gives a window of the map to a block simulated by `msim`.
`mvplat` starts `msim` with the arguments in `msim` (paths beside the platform
file are found), adding `mvp/sv/mvp_vp.sv` and `mvp/mvp_vp.c`; the testbench
instances `mvp_vp_master`, an AXI4-Lite master, wires it to the block and
drives a clock and a reset:

```systemverilog
mvp_vp_master #(.ADDR_W(32), .DATA_W(32), .IRQ_N(1)) vp (
    .aclk(clk), .aresetn(rst_n),
    .awaddr, .awprot, .awvalid, .awready, .wdata, .wstrb, .wvalid, .wready,
    .bresp, .bvalid, .bready, .araddr, .arprot, .arvalid, .arready,
    .rdata, .rresp, .rvalid, .rready, .irq(irq));
```

The two simulators run in lock step over a socket pair, with no wall clock in
between:

- At every quantum the platform tells the RTL to run up to the platform's
  time; the RTL answers with its interrupt lines (PLIC sources `irq` to
  `irq + irq_lines - 1`).
- When a hart reaches the window, the RTL is first brought to the hart's
  time, then performs the access as an AXI4-Lite transaction on the bus
  width, with byte strobes for narrow writes. The hart waits for the RTL's
  answer, and its own clock moves on by the time the transaction took.
  `SLVERR` and `DECERR` arrive as access faults.
- When the program ends, the platform ends the simulation with `$finish`, so
  `final` blocks run, and `msim`'s exit code is checked: if it is not 0, the
  run fails.

**DMA from RTL.** A bus master in the RTL (a DMA engine, a second CPU)
reaches the platform's memory map through `mvp_vp_slave`, an AXI4-Lite slave
in the same file:

```systemverilog
mvp_vp_slave #(.ADDR_W(32), .DATA_W(32)) mem (
    .aclk(clk), .aresetn(rst_n),
    .awaddr, .awprot, .awvalid, .awready, .wdata, .wstrb, .wvalid, .wready,
    .bresp, .bvalid, .bready, .araddr, .arprot, .arvalid, .arready,
    .rdata, .rresp, .rvalid, .rready);
```

Each transfer is one bus word with its byte strobes. The platform serves it
at once, from RAM, ROM or a peripheral, at the RTL's time. An address that
nothing maps, a write to ROM, or a device that refuses the access answers
SLVERR. The platform address is the AXI address plus the device's
`dma_offset` (default 0). As with any other store, a hart executes code that
DMA wrote only after `FENCE.I`.

`rtl_base` is the address the RTL sees for the window's first byte (default
0). One `rtl` device per platform. `MVPLAT_MSIM` (or `msim_path` in the
device) names the simulator when it is not on `PATH`.

## Booting an operating system

mvplat boots firmware and a kernel the way QEMU's `virt` board does, with the
same options:

```sh
mvplat -platform board.json -bios fw_dynamic.elf -kernel Image \
       -initrd root.cpio -append "console=ttyS0 rdinit=/init" -harts 4
```

- `-bios FILE` is the firmware, run first in machine mode on every hart:
  OpenSBI's `fw_dynamic` (an ELF or a raw binary; position-independent
  firmware linked at 0 runs from the start of RAM). With `-kernel` and no
  `-bios`, mvplat takes `$MVPLAT_BIOS` or the `fw_dynamic.elf` that the
  distribution's `opensbi` package installs.
- `-kernel FILE` is the kernel: a raw `Image` goes at RAM + 2 MiB (4 MiB on
  RV32), an ELF where it is linked. The firmware starts it in supervisor mode.
- `-initrd FILE` goes just below the device tree; `-append TEXT` becomes
  `/chosen/bootargs`.
- Every hart starts at the firmware with `a0` = hart id, `a1` = the device
  tree and `a2` = OpenSBI's `struct fw_dynamic_info` (next address, next mode
  = supervisor, any boot hart).

The device tree is made from the platform: a `cpu` node per hart (with
`riscv,isa`, `mmu-type` and its interrupt controller), a `memory` node per
RAM, and the CLINT (`riscv,clint0`), PLIC (`riscv,plic0`), each 16550 UART
(`ns16550a`; the first is `stdout-path`), each virtio device (`virtio,mmio`)
and the finisher (`sifive,test1` with
`syscon-poweroff` and `syscon-reboot`), with the compatible strings that
OpenSBI, Linux, U-Boot and Zephyr already have drivers for. It sits at the top
of the first RAM on a 2 MiB boundary. `-dump-dtb FILE` writes it out
(`dtc -I dtb -O dts FILE` shows it), and `-dtb FILE` passes another one
instead.

Checked: OpenSBI 1.7 (Ubuntu 24.04's `opensbi` package) starts on 1 to 8
harts, finds the UART, the ACLINT timer and IPIs, the PLIC and the syscon
reset from the tree, and starts the other harts through SBI HSM. Ubuntu
24.04's stock riscv64 kernel (6.8, `tests/vplat/get-linux.sh` fetches it)
boots to `/init` in an initramfs on 1 to 4 harts, with its console on the
16550 through the 8250 driver and interrupts from the PLIC, and powers the
board off (exit code 0). One hart takes about 870 million instructions, 20 s
on the machine this was measured on. `tests/vplat/linux_init.c` is the
`/init` the test uses (`tests/vplat/build.sh` builds it and the initramfs).
Ubuntu's static BusyBox, built for `rv64gc` (floating point included), runs
as the user space: its shell reads what `-input` types and runs it.
`-input` with `-kernel` or `-bios` holds the characters back until the
kernel's driver turns the receive interrupt on (a driver empties the FIFO
when it sets the port up, so earlier characters would be lost), as if they
were typed once the system is up.

## Debugging

`-trace FILE` writes a commit log, one line per instruction with the
register written and the memory touched, in spike's format, so a run can be
compared line by line with another simulator.

*(extension)* `-gdb PORT` waits for gdb before the first instruction (`-gdb 0` takes any
free port and `-gdb-portfile FILE` writes it down). gdb sees one thread per
hart and needs no setup: the server sends the `riscv:rv32` or `riscv:rv64` target
description, with the machine CSRs. Supported: breakpoints (`Z0`, `Z1`),
watchpoints on write, read and access (`Z2`, `Z3`, `Z4`), single step,
continue, Ctrl-C, register and memory reads and writes, detach and kill. An `ebreak` in the program stops in gdb rather
than trapping when gdb is attached. The platform is all-stop.

*(extension)* `-profile FILE` (or `-` for stdout) writes the instruction mix of every hart,
the hot functions (from the ELF symbols), and the accesses each peripheral
received.

## Checkpoints *(extension)*

`-save FILE` saves the platform when the run stops: every hart, every
device, all of memory (only the pages that hold something). Use it with
`-max-cycles` to stop after a boot, and `-restore FILE` to start any number of
later runs from there; a restored run continues exactly as the original
would have. `-restore` checks that the platform file describes the same
board. A platform with an `mvp` library or an `rtl` window cannot be saved:
their state lives outside the platform, and the save says so.

### Disks

`-disk FILE` puts a raw disk image on a `virtio-blk` device (Linux's
`/dev/vda`; repeat it for `vdb`, `vdc`, ...), in the virtio-mmio slots of
QEMU's virt board: `0x10001000` up, `0x1000` apart, PLIC sources 1 up. The
device tree gets a `virtio,mmio` node for each. The guest's writes stay in
the run (the image is mapped copy-on-write) unless `-disk-write` lets them
reach the file, or the platform file says `"write": true`. A request is
served when the driver notifies the queue, before the notifying store
retires: an instant disk, so a run stays deterministic.

```
mkfs.ext4 -d rootdir root.img 64M
mvplat -platform linux.json -bios fw_dynamic.elf -kernel Image -disk root.img \
       -append "console=ttyS0 root=/dev/vda rw"
```

Checked: Ubuntu 24.04's kernel mounts an ext4 image on `/dev/vda`, reads and
writes it (`debugfs` sees the written file with `-disk-write`), and boots
with its root file system on the disk and no initramfs.
`tests/vplat/virtio.c` is a bare-metal driver that tests the device itself:
reads, a two-sector read, a write read back, flush, `GET_ID`, a read past the
end (`IOERR`), an unknown request (`UNSUPP`) and the interrupt.

### Networks

`-net user` puts a `virtio-net` interface (Linux's `eth0`; repeat for
`eth1`, ...) in the next virtio-mmio slot, with MAC `52:54:00:12:34:56` (the
last byte counts up). Its back end is a built-in gateway at `10.0.2.2`
(`52:54:00:12:35:02`) that answers ARP and ICMP echo: enough to bring an
interface up and ping, with nothing on the host and the same result every
run. It forwards nothing beyond that (no TCP/UDP to the host or the internet
yet).

`-net dgram:LOCAL,REMOTE` connects the interface to Unix datagram sockets,
one Ethernet frame per datagram: it binds `LOCAL` and sends to `REMOTE`. Two
platforms given each other's paths share a link; a program on the host can
be the other end (`tests/vplat/echo_peer.py` is the one the test uses).
Frames that arrive are taken every 100 us of simulated time. `-net-pcap FILE`
records the first interface's frames, both ways, for Wireshark or tcpdump.

```
mvplat -platform linux.json -bios fw_dynamic.elf -kernel Image -initrd rootfs.cpio -net user \
       -append "console=ttyS0" -input "ifconfig eth0 10.0.2.15 up; ping -c 2 10.0.2.2"
```

(Ubuntu's kernel has no `ip=` autoconfiguration built in, so the address is
set from user space.)

Checked: Ubuntu 24.04's kernel brings `eth0` up and BusyBox's `ping`
reaches the gateway. `tests/vplat/vnet.c` is a bare-metal driver that
checks the device itself (ARP, ICMP checksums, the interrupt, the dgram
link). A platform with a `dgram` interface cannot be checkpointed.

### A virtio console

`-vcon` adds a `virtio-console` in the next virtio-mmio slot and sends
`-input` to it instead of the first UART. Linux uses it with
`console=hvc0`. Input waits until the guest has printed something and then
been quiet for 100 ms of simulated time, as a person types at a prompt:
characters that arrive while a system is still starting are lost to it, and
virtio has no register that says the driver is ready to read, as the 16550's
receive interrupt enable does. `tests/vplat/vcon.c` is a bare-metal driver
for it.

## Options

| Option | |
| --- | --- |
| `-platform FILE` | the platform (default: the built-in `virt32`) |
| `-elf FILE` | the program (or give it as the first plain argument) |
| `-harts N` | override the platform's hart count |
| `-threads N`, `-j N` | threads for the harts (default: every CPU) |
| `-quantum N` | cycles between synchronisations (default 1000) |
| `-mhz F` | hart clock |
| `-max-cycles N` | stop after N cycles, exit code 4 |
| `-input TEXT` | what the first UART receives |
| `-trace FILE` | commit log |
| `-bios FILE`, `-kernel FILE`, `-initrd FILE`, `-append TEXT` | boot firmware and a kernel (see "Booting an operating system") |
| `-dtb FILE`, `-dump-dtb FILE` | pass another device tree; write the platform's and exit |
| `-disk FILE`, `-disk-write` | a raw disk image on virtio-blk; let writes reach the file (see "Disks") |
| `-vcon` | a virtio console; `-input` goes to it (see "A virtio console") |
| `-net user`, `-net dgram:LOCAL,REMOTE`, `-net-pcap FILE` | a virtio-net interface and its back end; record its frames (see "Networks") |
| `-profile FILE` | profile (`-`: stdout) *(extension)* |
| `-gdb PORT`, `-gdb-portfile FILE` | gdb server *(extension)* |
| `-save FILE`, `-restore FILE` | checkpoints *(extension)* |
| `-map` | print the memory map |
| `-q` | no summary line |

Exit codes: the program's own; 2 for a usage, platform or model error; 3
when every hart waits for an interrupt nothing can send; 4 at `-max-cycles`.

## Performance

Measured on the container this was developed in (4 cores), with
`tests/vplat/bench.c` (a sieve and a CRC per hart, no sharing):

| Run | Speed |
| --- | --- |
| 1 hart | 150 to 170 MIPS |
| 4 harts, 4 threads | 190 to 200 MIPS in all |

Each hart keeps a cache of decoded instructions (8192 entries, by address),
so an instruction is fetched, expanded from its compressed form and split
into fields once. As the ISA allows, a hart sees a store to code only after
it executes `FENCE.I`; loading a program, a debugger write and `-restore`
empty the cache too. With paging on, the page that instructions were last
fetched from stays translated and PMP-checked until the next `SFENCE.VMA`,
`satp` or PMP write or change of privilege. Loads and stores with paging on
keep the same for the last 64 pages of plain memory each read and wrote
(a page any part of which is a device, is only partly allowed by PMP, or holds
`tohost` takes the general path). Linux boots at about 50 MIPS. Loads and
stores to plain memory with nothing to check
(no PMP in force, no watchpoint, no write buffer) take an inline path. RAM is
mapped on first touch, so a large memory costs nothing until it is used. A
co-simulated RTL access costs two socket round trips plus the RTL's own
simulation time.

## Limits / Not yet

- No V, no B (bit-manipulation), no Zfh, no Sv48/Sv57, no Svadu (hardware
  A/D update), and every hart of a platform has the program's width. F and D
  round to nearest-ties-to-max-magnitude (RMM) exactly only in conversions
  to integers; other RMM results round as ties-to-even (differs only for a
  value exactly half way). Bare-metal software, RTOSes (FreeRTOS, Zephyr),
  OpenSBI, Linux with an rv64gc user space and the architecture tests run.
- Of virtio, the block, network and console devices: no entropy, 9p,
  GPU or input devices yet, no console multiport, and no indirect descriptors, `EVENT_IDX`, checksum or segmentation
  offload, or multiqueue. Disk images are raw (no qcow2). The `user` network
  answers ARP and ping only; there is no NAT to the host or a TAP back end.
- No hardware triggers, no Smepmp (`mseccfg`).
- One instruction per cycle: no pipeline or cache timing model.
- No Spike or QEMU back end yet (Spike is BSD-licensed and could be linked;
  QEMU is GPL and could only run as a separate program).
- One `rtl` window per platform, AXI4-Lite only (no AXI4 bursts, no
  outstanding transactions); a DMA transfer is one bus word.
- No IP-XACT import, no GUI; platform files are JSON.
- Checkpoints exclude `mvp` libraries and RTL windows.
- The commit log follows spike's layout but has not been compared with spike
  line by line in CI.
