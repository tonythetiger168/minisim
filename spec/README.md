# Specifications

Specifications that minisim's verification IP is checked against. Only the
references are kept here, not the documents themselves: most of them may not
be redistributed. Download each one from its publisher and put it in this
folder; `.gitignore` keeps the PDFs out of the repository.

| Document | Version | Used by | Get it from |
|---|---|---|---|
| AMBA AXI and ACE Protocol Specification | ARM IHI 0022H.c (January 2021) | `vip/axi4` | [developer.arm.com/documentation/ihi0022/hc](https://developer.arm.com/documentation/ihi0022/hc) |

The IEEE standards behind msim itself (1800, 1800.2, 1801, 1735, 1076) are
tracked in `docs/COVERAGE.md` and the master spec table, not here.

## AXI4 VIP against IHI0022H.c

`vip/axi4` implements the AXI4 interface (not AXI3, AXI5 or ACE). The tables
list each requirement of the specification, where the VIP meets or checks it,
and the test that proves it. Rule names are those `axi4_sva` reports, as
`[<section> <RULE>]` at the start of the message; `tests/vip/axi4_sva` makes
each of them fire once, and the loopback benches show they stay silent on
legal traffic.

### Signals (chapter A2, A8)

| Section | Requirement | VIP |
|---|---|---|
| A2.1-A2.6 | Global, AW, W, B, AR and R channel signals | `axi4_if`: all AXI4 signals; AWLOCK/ARLOCK one bit (A7.4) |
| A2.3 | Data bus 8 to 1024 bits; one WSTRB bit per byte | `AXI4_DATA_W` (default 32), `AXI4_ADDR_W`, `AXI4_ID_W`, `AXI4_USER_W` in `axi4_defines.svh`; `tests/vip/axi4w` runs 128-bit data, 40-bit address |
| A8.1 | AWQOS/ARQOS | driven, captured, compared by both monitors, covered |
| A8.2 | AWREGION/ARREGION | driven, captured, part of the exclusive-sequence match (A7.2.4), covered |
| A8.3 | AWUSER, WUSER, BUSER, ARUSER, RUSER | driven per burst / per beat; the slave echoes AxUSER on BUSER/RUSER; checked by the scoreboard |
| A9.3 | Default values | the master drives 0 on QOS/REGION/USER unless a sequence sets them |

### Clock, reset and handshakes (A3.1, A3.2)

| Section | Rule | Checked by |
|---|---|---|
| A3.1.2 | VALIDs LOW during reset | `A3.1.2 VALID_IN_RESET` |
| A3.1.2 | ARVALID/AWVALID/WVALID LOW on the first edge after reset | `A3.1.2 VALID_AT_RESET_EXIT` |
| A3.2.1 | Once VALID is HIGH it stays HIGH until the handshake | `A3.2.1 {AW,W,B,AR,R}_VALID_STABLE` |
| A3.2.1 | Information stays stable while VALID is HIGH without READY | `A3.2.1 {AW,W,B,AR,R}_STABLE` (all payload signals, QOS/REGION/USER included) |
| A3.2.1 | VALID and READY are never X; payload not X while VALID | `A3.2.1 {AW,W,B,AR,R}_HS_X`, `A3.2.1 {AW,W,B,AR,R}_X` |
| A3.2.2 | A source does not wait for READY before VALID | the drivers raise VALID without looking at READY |

### Channel relationships (A3.3)

| Section | Rule | Checked by |
|---|---|---|
| A3.3 | Write data may come before its address | master `w_thread` runs ahead of `aw_thread`; monitor and checker pair early W beats |
| A3.3.1 | BVALID only after the AW and the last W handshake of the write | `A3.3.1 B_BEFORE_WRITE` |
| A3.3.1 | RVALID only after the AR handshake of the read | `A3.3.1 R_BEFORE_AR` |

### Bursts and data (A3.4)

| Section | Rule | Checked by |
|---|---|---|
| A3.4.1 | AxBURST 0b11 is reserved | `A3.4.1 {AW,AR}_BURST_RSVD` |
| A3.4.1 | WRAP length 2, 4, 8 or 16 | `A3.4.1 {AW,AR}_WRAP_LEN` |
| A3.4.1 | WRAP start address aligned to the transfer size | `A3.4.1 {AW,AR}_WRAP_ALIGN` |
| A3.4.1 | FIXED and WRAP at most 16 beats; INCR up to 256 | `A3.4.1 {AW,AR}_LEN_FIXED_WRAP`; sequences use INCR up to 256 |
| A3.4.1 | Transfer size not wider than the bus | `A3.4.1 {AW,AR}_SIZE` |
| A3.4.1 | A burst does not cross 4 KB | `A3.4.1 {AW,AR}_4K` |
| A3.4.1 | No early termination; WLAST/RLAST on the last beat only | `A3.4.1 WLAST`, `A3.4.1 RLAST`; monitor beat counts |
| A3.4.2 | Beat addresses of FIXED, INCR, WRAP; narrow and unaligned transfers | `axi4_item::beat_addr_of`; scoreboard checks every byte against a shadow memory |
| A3.4.4 | WSTRB HIGH only on lanes with valid data | `A3.4.4 WSTRB_LANES` (also for W beats ahead of their AW) |
| A3.4.4 | Data on the active lanes is valid | `A3.4.4 WDATA_X`, `A3.4.4 RDATA_X` |
| A3.4.5 | OKAY, EXOKAY, SLVERR, DECERR | slave `err_ranges` give SLVERR/DECERR; scoreboard checks each response |

### Attributes (A4)

| Section | Rule | Checked by |
|---|---|---|
| A4.4 Table A4-5 | AxCACHE values outside the table are reserved | `A4.4 {AW,AR}_CACHE_RSVD`; sequences pick from `AXI4_CACHE_LEGAL` |
| A4.7 | AxPROT | driven and captured; all eight values covered |

### Transaction identifiers and ordering (A5, A6)

| Section | Requirement | VIP |
|---|---|---|
| A5.1, A6.5 | Responses with one ID in order; different IDs in any order | slave `cfg.reorder`; master and monitor match each response to the oldest burst with its ID; `vip_axi4_outstanding_test` requires out-of-order responses |
| A5.1 | Read data of different IDs may interleave; no write data interleaving in AXI4 | slave `cfg.interleave`; master sends W bursts in AW order; the test requires interleaved beats |
| A6 | Several transactions outstanding | master `cfg.max_outstanding`; the random sequence holds back a burst that overlaps one in flight (hazard), so results stay defined |

### Exclusive access (A7.2)

| Section | Rule | Checked by |
|---|---|---|
| A7.2.1-A7.2.3 | Exclusive read sets a monitor per ID; any write to the location clears it; exclusive write succeeds (EXOKAY, memory written) only if its monitor holds, else OKAY and memory untouched; a new exclusive read of the same ID moves the monitor | slave exclusive monitor; `vip_axi4_excl_test` cases 1-5 |
| A7.2.2 | The write part starts after the read part completes | `axi4_api::excl_*`, `axi4_random_seq` |
| A7.2.3 | A slave without exclusive support answers OKAY and writes memory | `cfg.exclusive_support = 0`; `vip_axi4_excl_test` case 6 |
| A7.2.4 | Total size a power of two up to 128 bytes, at most 16 beats, address aligned to the total size | `A7.2.4 {AW,AR}_EXCL` |
| A7.2.4 | Read and write of a sequence have the same ID, address, length, size, burst, cache, prot, region | slave match; `axi4_api::excl_write` copies them |
| A7.2.5 | EXOKAY only for exclusive accesses | `A7.2.5 B_EXOKAY`, `A7.2.5 R_EXOKAY`, monitor |
| A7.2.5 | An exclusive read does not mix OKAY and EXOKAY | `A7.2.5 R_EXCL_MIXED`, monitor |
| A7.3 | Locked accesses do not exist in AXI4 | AxLOCK is one bit |

### Recommendations

| Rule | Checked by |
|---|---|
| READY within a bounded time | `REC {AW,W,B,AR,R}_MAX_WAIT` warnings when `axi4_sva` has `MAX_WAIT > 0` |

### Not covered

- AXI3 (WID, four-bit AxLEN, two-bit AxLOCK, locked transactions), AXI5
  and ACE signals, AXI4-Stream.
- A3.1.1: no combinational path between inputs and outputs, a structural
  property of the RTL that a bus monitor cannot see.
- A8.2: AxREGION constant within a 4 KB space, which needs the system
  address map.
- Memory-type behaviour of A4 (buffering, caching, ordering of Device
  accesses) is not modelled by the memory responder: it is a plain memory.
- `vip/axi4lite` is a separate VIP and is not covered by these tables.
