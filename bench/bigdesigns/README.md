# Large open-source designs on msim

Scripts that fetch real, large designs, build them as their own projects do, and run
msim on them next to Verilator 5.054. Nothing from these projects is committed here:
each script clones the source into `$BIG` (default `~/bigdesigns`) and generates what it
needs. Results and the msim issues found are tracked in the project report
(`reports/bigdesigns/README.md` in the shared project folder).

| script | design | licence of the design (test input only) |
|---|---|---|
| `xiangshan.sh gen / elab / run` | XiangShan (Chisel; MinimalConfig by default, `CONFIG=`) | Mulan PSL v2 |
| `c910.sh` | T-Head OpenC910 with its smart_run testbench | Apache-2.0 |
| `cva6.sh elab / run` | CVA6 (Ariane) core, and the corev_apu UVM testbench | Solderpad 0.51 |
| `nvdla.sh` | NVDLA nv_full | NVIDIA Open NVDLA License |
| `openpiton.sh X Y` | OpenPiton X×Y mesh (OpenSPARC T1 tiles) | BSD; T1 core GPL-2.0 |
| `opentitan.sh` | OpenTitan Earl Grey `chip_sim_tb` | Apache-2.0 |

Helpers: `tm` prints wall time and peak memory of a command; `flatten_flist.py` expands
`$VARS` and nested `-f`/`-F` in a VCS-style file list. `xs_dpi.cpp` / `tb_xs.sv` replace
XiangShan's Verilator-only DiffTest harness with RAM, flash and trap handling in DPI-C
(`gen_dpic_stubs.py` writes empty bodies for the other DiffTest DPI calls), and
`cva6/elf_dpi.c` replaces CVA6's Spike-based ELF loader.

Needs: clang + lld (RISC-V bare-metal programs), a JDK and mill 0.12.17 for XiangShan,
`csh` and `libbit-vector-perl` for OpenPiton, `libyaml-perl libio-tee-perl
libcapture-tiny-perl` for NVDLA, Python venv for OpenTitan's fusesoc. On a 16 GB machine
run one design at a time: XiangShan alone takes about 10 GB in msim.

XiangShan needs the reset held for about 100 cycles (`tb_xs.sv +reset_cycles=`, set by the
script). With 10 cycles the CSR's sticky `criticalErrorState` latches random initial values
right after reset and the ROB never commits; Verilator built with `--trace` hangs the same way.
