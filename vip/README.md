# minisim VIP library

UVM verification IP that runs on `msim -uvm`. Each VIP has an interface with
clocking blocks, an agent (master or slave role, active or passive), a
monitor, functional coverage, protocol assertions and sequences; the slave
side is a reactive memory responder on the shared memory model.

| VIP | Directory | Contents |
|---|---|---|
| AXI4 | `axi4/` | `axi4_if`, `axi4_pkg` (agent, master driver, memory responder, monitor with several outstanding bursts and W before AW, coverage, random and directed sequences), `axi4_sva` |
| AXI4-Lite | `axi4lite/` | `axil_if`, `axil_pkg` (agent, master driver with AW/W order and skew, memory responder, monitor, coverage, scoreboard, sequences), `axil_sva` |
| APB4 | `apb/` | `apb_if`, `apb_pkg` (requester, completer with wait states and a PSLVERR window, monitor, coverage, sequences), `apb_sva` |
| Memory | `mem/` | `mem_vip_pkg::mem_model`: sparse byte memory with bus-word access and a backdoor, shared by the responders |

The AXI4, APB4 and memory VIPs come from
[SoC-tb-regress-builder](https://github.com/tonythetiger168/SoC-tb-regress-builder)
(`vip/`), with two method names changed so they compile under a strict
front end (see `docs/VIP.md`). All files here are Apache-2.0, as there.

Self-test benches are in `tests/vip/`; `docs/VIP.md` shows how to run them
and how to run the larger
[open-vip-portfolio](https://github.com/tonythetiger168/open-vip-portfolio)
on msim with `tools/mvip.py`.
