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

**Harts.** RV32GCBV or RV64GCBV (IMAFDC with Zicsr and Zifencei, the B
extension's Zba, Zbb and Zbs, Zbc, Zicond, the scalar-crypto bit
manipulation Zbkb and Zbkx, the byte and halfword atomics Zabha, load-acquire
and store-release Zalasr (LB..LD.AQ, SB..SD.RL and their .AQRL forms;
misaligned addresses raise address-misaligned exceptions), compare and
swap Zacas (AMOCAS.W/D, AMOCAS.Q on RV64 and AMOCAS.D on RV32 with register
pairs, AMOCAS.B/H), half-precision floating point Zfh (every operation,
rounding mode and flag, NaN-boxed in the F registers), BFloat16 Zfbfmin
(FCVT.BF16.S and FCVT.S.BF16, with FLH, FSH, FMV.H.X and FMV.X.H from Zfh),
and the cache-block operations CBO.ZERO, CBO.CLEAN,
CBO.FLUSH and CBO.INVAL with 64-byte blocks; there are no caches, so the
last three only check privilege), in machine,
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

**Vectors.** The V extension 1.0: VLEN 128 bits unless `-vlen` (or the
platform's `"vlen"`) says 256, 512 or 1024, ELEN 64. `vsetvl`, `vsetvli`
and `vsetivli` with every SEW from 8 to 64 and LMUL from 1/8 to 8 (a
reserved combination sets `vill`); unit-stride, strided, indexed (ordered
and unordered), segment (2 to 8 fields), whole-register, mask and
fault-only-first loads and stores; the integer, fixed-point (`vxrm`'s four
rounding modes, `vxsat`), mask, reduction and permutation instructions; and
vector floating point at SEW 32 and 64 with the scalar FPU's rounding,
flags and NaNs, the widening and narrowing forms (round-to-odd included),
and `vfrec7`/`vfrsqrt7` from the specification's tables. Tails and inactive
elements are left undisturbed, which the agnostic policies allow. A load or
store that faults leaves `vstart` at the element and `mtval` at its address;
a fault-only-first load past element 0 trims `vl` instead. `mstatus.VS`
works as `FS` does (off until software turns it on, dirty after a write,
with `SD`); `vstart`, `vxsat`, `vxrm`, `vcsr`, `vl`, `vtype` and `vlenb`
are there. Linux turns the vector unit on for the processes that use it.
Zvbb (with Zvkb, its subset) adds `vandn`, `vror`/`vrol` (6-bit `vror.vi`),
`vbrev8`, `vrev8`, `vbrev`, `vclz`, `vctz`, `vcpop.v` and the widening
`vwsll`, checked at every SEW by `tests/vplat/zvbb.c`. Zvfh (with
Zvfhmin) makes SEW 16 half precision for every vector floating-point
instruction, widening to single and narrowing from it, and the conversions
to and from 8, 16 and 32-bit integers; `tests/vplat/zvfh.c` checks each one
against the scalar Zfh instruction in all five rounding modes, flags
included. BFloat16 vectors at SEW 16: Zvfbfmin (`vfncvtbf16.f.f.w`,
`vfwcvtbf16.f.f.v`) and Zvfbfwma (`vfwmaccbf16.vv/.vf`, BFloat16 sources
widened exactly and fused into a single-precision accumulator);
`tests/vplat/bf16.c` checks FCVT.BF16.S against a rounding model written
from the specification and each vector form against the scalar ones.
(QEMU 8.2 reads an F register that is not NaN-boxed as half precision's
canonical NaN, 0x7e00, in FCVT.S.BF16; mvplat uses BFloat16's, 0x7fc0.)
Vector cryptography: Zvkned (AES rounds, `vaesz`, the AES-128 and
AES-256 key schedules), Zvknha and Zvknhb (SHA-256 and SHA-512 message
schedule and rounds), Zvkg (GHASH), Zvksed (SM4 rounds and key
schedule), Zvksh (SM3 message expansion and compression), Zvbc (`vclmul`,
`vclmulh`) and Zvkt; `tests/vplat/zvk.c` checks them on FIPS-197, FIPS
180-4, GCM, GB/T 32907 and GB/T 32905 published answers.

**Supervisor mode and the MMU.** `medeleg` and `mideleg` hand exceptions
and interrupts to supervisor mode; `sstatus`, `sie` and `sip` are the
supervisor's views of the machine registers, with `stvec`, `sscratch`,
`sepc`, `scause`, `stval`, `scounteren` and `satp`. SRET, SFENCE.VMA and the
`mstatus` bits SUM, MXR, TVM, TW and TSR work as the privileged
specification says. `satp` selects Sv32 paging on RV32 (two levels, 4 MiB
megapages) and Sv39, Sv48 or Sv57 on RV64 (three, four or five levels, with
superpages at every level; a virtual address not sign-extended from bit 38,
47 or 56 faults), with a 256-entry TLB per hart
and page faults (causes 12, 13 and 15). No ASIDs (`satp.ASID` reads 0). Svnapot
64 KiB pages on RV64 (`N` on a level-0 leaf whose PPN ends in `1000`);
Svpbmt on RV64: with `menvcfg.PBMTE` set a leaf may say PMA, NC or IO
(the same here, as there are no caches); PBMT 3, any PBMT with PBMTE clear,
or a PBMT on a pointer PTE faults. `menvcfg.CBZE` and `senvcfg.CBZE` read 1,
so CBO.ZERO works in every mode; CBCFE and CBIE are 0, so the others are
machine-mode only. Accessed and dirty bits follow Svade: a page whose A bit is clear,
or a store to a page whose D bit is clear, faults and the supervisor sets
them, as spike and QEMU do by default. The page walk is checked by PMP.
The PLIC has two contexts per hart, as QEMU's `virt` does: context 2*h*
interrupts hart *h* in machine mode (MEIP) and 2*h*+1 in supervisor mode
(SEIP).

The RISC-V architecture tests all pass, 837 programs: ui, um, ua, uc, uf,
ud, uzba, uzbb, uzbc, uzbs, uzicond, uzbkb, uzbkx, uzabha, uzacas,
uzabha_zacas and uzfh for RV32 and RV64, both bare (`p`) and under the riscv-tests supervisor with
paging on (`v`, Sv32 and Sv39, and RV64's ui, um, ua and uc under Sv48
too), mi and si for both, and RV64's mzicbo and ssvnapot; and the vector
benchmarks vec-daxpy, vec-memcpy, vec-sgemm and vec-strcmp at VLEN 128 and
512, 845 runs. `tests/mvplat_isa.sh` runs them. riscv-tests has no
instruction-level vector suite, so `tests/mvplat.sh` adds two: loops that
clang vectorizes, each compared with the same loop compiled scalar (at VLEN
128 and 1024, with LMUL 1 to 8 groups), and the
instructions a vectorizer seldom emits (slides, gathers, compress, fixed
point in every rounding mode, segments, strides, fault-only-first, `vstart`
after a trap, `vsetvl` corners, the estimates) against scalar models; both
on RV32 and RV64; together they use 226 distinct vector instruction forms.
The vector loops run under Linux too, in three processes at once.

**Time.** Each hart runs a quantum of cycles (`-quantum`, default 1000, 10000 with `-serial`) on
its own and then the platform synchronises: devices see time move and
interrupts are re-evaluated. This is SystemC TLM-2.0's temporal decoupling.
A quantum ends early at the next timer compare and at the next callback an
mvp model has armed, so timer interrupts and model delays arrive on time
whatever the quantum.

**Several harts, several threads.** With `-parallel` (or `-threads N`, or
`"serial": false` in the platform file) harts run in parallel rounds, one
per thread (`-threads N`, default every CPU). Each hart's stores go to its own write
buffer during a round and are written back in hart order at its end;
accesses with side effects (peripheral registers, atomics, LR/SC, HTIF) wait
for the ordered phase of the round and happen in hart order there. The run
therefore does not depend on the thread count or on the operating system's
scheduler: the console, the commit log and the profile are the same, byte
for byte, on 1 thread and on 64. The test suite checks exactly that.

**Taking turns** is the default with several harts (`-serial`, or
`"serial": true` in the platform file, says so explicitly): the harts take
turns on one thread, each running to the end of the
quantum, and store to memory directly, with no write buffers and no
ordered phase. That is another interleaving than the parallel rounds give
(each hart sees the others' stores of earlier turns at once), still the
same on every run. Every hart then runs on the fast loop and the
translator, where a write buffer would hold them to the general path, so
an SMP operating system runs about as fast as on one hart. The harts also
share one translation of their paged (supervisor and user) code, each with
its own table of indirect jumps, so the kernel is translated once rather
than once per hart, and the quantum is 10000 cycles unless `-quantum` or
the platform file says otherwise (a turn costs a switch; with no thread
waiting on another, longer turns are cheaper). Linux on 2 harts boots to
the BusyBox shell and powers off in about 1.4 s with `-serial`, level with
QEMU 8.2's virt board, against 20 s in parallel rounds, which is why turns
are the default. A hart's LR reservation
ends with its turn (an SC after the other harts ran fails, and the loop
around it tries again).

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
  "vlen": 128,               // vector register bits: 128 (default), 256, 512, 1024
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
description, with the machine CSRs and the vector unit (`v0`-`v31` at
VLEN bits and the vector CSRs). Supported: breakpoints (`Z0`, `Z1`),
watchpoints on write, read and access (`Z2`, `Z3`, `Z4`), single step,
continue, Ctrl-C, register and memory reads and writes, detach and kill. An `ebreak` in the program stops in gdb rather
than trapping when gdb is attached. The platform is all-stop. Checkpoints
keep the vector registers, `vl`, `vtype`, `vstart`, `vxrm` and `vxsat`; a
checkpoint restores only at the VLEN it was saved with.

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
| `-quantum N` | cycles between synchronisations (default 1000; 10000 with `-serial`) |
| `-serial` | harts take turns on one thread, no write buffers (the default with several harts; see above) |
| `-parallel` | parallel rounds with write buffers, on `-threads` threads (also implied by `-threads`) |
| `-mhz F` | hart clock |
| `-max-cycles N` | stop after N cycles, exit code 4 |
| `-input TEXT` | what the first UART receives |
| `-trace FILE` | commit log |
| `-bios FILE`, `-kernel FILE`, `-initrd FILE`, `-append TEXT` | boot firmware and a kernel (see "Booting an operating system") |
| `-dtb FILE`, `-dump-dtb FILE` | pass another device tree; write the platform's and exit |
| `-mmu sv39\|sv48\|sv57` | the largest paging mode the device tree offers an RV64 OS (default `sv39`; the platform file's `"mmu"` too). The harts take Sv39, Sv48 and Sv57 whatever it says |
| `-vlen BITS` | the vector registers' length: 128 (default), 256, 512 or 1024 (the platform file's `"vlen"` too) |
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

Each hart keeps a cache of decoded instructions (262144 entries, by
physical address, so `SFENCE.VMA` and `satp` writes leave it alone),
so an instruction is fetched, expanded from its compressed form and split
into fields once. A hart in machine mode with no trace, profile,
breakpoint, watchpoint, single step, write buffer, PMP lock or MPRV runs a
fast loop: the common integer, branch, load and store instructions (RV32I,
RV64I, MUL and MULW) go straight from the cache, each handler jumping to
the next one's. A run of them up to a jump or branch becomes a basic block
(16384 per hart, up to 32 instructions each) that runs with no pc or budget
check inside; it is built from the cache and dropped with it. The same
loop runs supervisor and user code with paging on: instructions from pages
the hart has already fetched from at that privilege, loads and stores
through the data page caches below; a miss leaves for the general path,
which fills them. On this loop alone (`-nojit`) Linux boots to a BusyBox
shell in about 4.5 s. It leaves, before the instruction, at anything else: a
cache miss, a CSR, a device or `tohost` access, the last 7 bytes of a
memory region, the end of the quantum. A 2.1 G-instruction RV64IMAC
program (integer loops, a CRC, a sieve) runs at about 670 MIPS this way (136 before).
As the ISA allows, a hart sees a store to code only after
it executes `FENCE.I`. The hart keeps a copy of each page it decoded
instructions from (up to 512 pages), and `FENCE.I` drops only what came
from pages that differ from their copy, however they were changed (a store
on any path, DMA, another hart). Those pages are also read-only on the host
(Linux and macOS): the first store to one faults and marks it written, so
`FENCE.I` compares only pages written since it last looked
(`MVPLAT_NO_WTRACK=1` compares them all). Linux runs about 4,600 `FENCE.I`
while it boots, and they drop about 100 pages in all. Loading a program, a
debugger write and `-restore` empty the cache. With paging on, the page that instructions were last
fetched from stays translated and PMP-checked until the next `SFENCE.VMA`,
`satp` or PMP write or change of privilege. Loads and stores with paging on
keep the same for the last 256 pages of plain memory each read and wrote
(a page any part of which is a device, is only partly allowed by PMP, or holds
`tohost` takes the general path). Linux boots at about 180 MIPS. Loads and
stores to plain memory with nothing to check
(no PMP in force, no watchpoint, no write buffer) take an inline path. RAM is
mapped on first touch, so a large memory costs nothing until it is used. A
co-simulated RTL access costs two socket round trips plus the RTL's own
simulation time.

On x86-64 hosts (Linux, macOS) an RV64 hart that would run the fast loop
runs translated code instead (`src/vp/jit.hpp`; `-nojit` turns it off). A
block of up to 64 instructions (RV64I, M, Zbb, the compressed forms,
FENCE) becomes x86-64 code: up to seven of its busiest registers live in
host registers, a jump or branch to a known address goes straight to the
next block once that exists, and JALR looks its target up in a table. In
machine mode, loads and stores go straight to host memory after one range
check. In supervisor and user mode with paging on, a block is found by
virtual pc through the fetch page record and translated from the physical
address the pc has then (so another mapping of the same code gets its own
translation); it links straight only to blocks in its own page, and goes
to others through the table, which is emptied with the TLB (SFENCE.VMA
with an address empties only what lies in that address's 1 GiB, the
largest page that can map it with Sv39). Loads, stores,
the AMOs, LR and SC (one hart, or `-serial`) and CBO.ZERO look their page up in the
hart's data page caches inline; a miss, a device, an access across a page
or a misaligned AMO leaves for the general path. CSRRCI on sstatus in
supervisor mode (clearing SIE, which lets no interrupt in) is translated;
other CSR instructions leave. The budget is checked once per block; when
the quantum ends inside a block, a counting copy of that block runs the
rest exactly, so a run retires the same instructions on the same cycles
as with `-nojit`. Translations are dropped when FENCE.I finds a code page
changed, and with the decoded-instruction cache (a new program). With one hart and no device that
keeps time of its own, run() lets up to 64 quanta go by at once and cuts
back to the quantum boundary after the first device access, which is what
the quanta would have done.

With `bench/mvplat/run.sh` (best of 3, wall clock, QEMU 8.2 with TCG on
the same host), the translator brings CoreMark (4000 iterations) from 3.6 s
to 0.40 s (QEMU 0.78 s), Dhrystone from 2.1 s to 0.28 s (QEMU 0.54 s), and
each of the 17 Embench programs ahead of QEMU (0.04 to 0.19 s against 0.10
to 0.28 s). Linux (one hart) boots to the BusyBox shell and powers off in about
1.0 s, best of 5 (QEMU 1.29 s on the same host; 4.4 s with `-nojit`). Floating point and
vector instructions still run on the general path.

## Limits / Not yet

- No Zbkc,
  no Svadu (hardware A/D update), and every hart of a platform
  has the program's width. F and D
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
