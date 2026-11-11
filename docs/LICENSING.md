# Licensing

minisim is open core. The free edition is this repository, under the Apache
License 2.0. Paid tools are a separate, closed-source product that runs on
the same flow and is distributed under a commercial license; their source is
not in this repository.

## The free edition

Everything in this repository is Apache-2.0 (`LICENSE`), with attribution in
`NOTICE`. That means anyone may use it commercially, modify it, redistribute
it and ship products built with it, including inside a closed-source flow, as
long as they keep the license and notice and state what they changed. The
Apache license also grants a patent license, which MIT does not, and that is
why it is the one companies' legal teams are used to approving.

The free edition is not a demo. It is the whole verification flow and it has
no seat count, no node-locked key, no time limit and no telemetry.

### Why not MIT any more

Until 2026-11-11 the repository was MIT. The MIT grant is not withdrawn:
copies obtained under it stay under it, and so does the git history. Releases
from 0.3 onward are Apache-2.0, which adds the express patent grant and the
notice requirements that matter once a paid edition exists beside the free
one.

## The paid edition

A paid tool is shipped as a binary built from a private repository. It links
nothing from a copyleft project, reads and writes the same file formats, and
is driven by the same command lines, so a flow can use free and paid tools
side by side. Nothing in the free edition is crippled to sell the paid one:
no watermarked output, no design-size cap, no "evaluation" banner.

## Which tool is in which edition

**This table is a proposal and nothing has been moved.** It takes Tony's
decision to settle it, and until then every tool below is built from this
repository under Apache-2.0.

The line it draws is *verify free, implement and sign off paid*: the tools a
team needs to write and run a testbench are free, and the tools that sit in
an implementation flow next to a foundry PDK are paid.

| Edition | Tools |
|---|---|
| **Free (Apache-2.0, this repository)** | `msim` (simulator), `mwave` (VCD / FST), `mcov` (coverage), `mregress` (regression runner), `mplan` (verification planning), `mbug` (bug filing), `mlint` + CDC (lint), `mdft` (scan-readiness checks), `mvp` (virtual prototypes), `mdebug` (source debug and viewer) |
| **Paid (closed source, separate repository)** | `msynth` (synthesis), `mpnr` (place and route), `msta` (static timing), `mrcx` (parasitic extraction), `mequiv` (equivalence checking), `mformal` (formal property checking), `matpg` (test pattern generation) and `mdft` scan insertion / MBIST, `memu` + `mfpga` (emulation and FPGA prototyping), `mhls` (high-level synthesis), `mspice` + `mchar` (circuit simulation and library characterization), `mpv` (physical verification: DRC and LVS), `mpower` (power signoff), `msdc` (constraint checking), `mlibc` (library checking), `msimprotect` (IEEE 1735 IP protection and key management) |

Two things are worth saying plainly before anything moves:

1. **Moving a tool out closes its future, not its past.** Every tool listed
   as paid has already been published here under MIT, so the code as it
   stands today can be forked by anyone, for ever. Closing a tool only keeps
   later work private.
2. **The split costs engineering.** The paid tools share `src/` headers with
   the free ones (`lintir.hpp`, `sta.hpp`, `aig.hpp`, the Tcl and SDC
   readers, the parallel runner). Whatever is shared has to stay in the free
   repository as a library the private one builds against, or be duplicated.
   The first is the cheaper answer and keeps the free edition honest.

## Rules for contributions

- **No GPL, LGPL, AGPL or other copyleft code**, in either edition, in
  source, in a vendored copy or as a linked dependency. Permissive licenses
  only: MIT, BSD, Apache-2.0, Boost, zlib, ISC.
- A new third-party dependency goes in `THIRD_PARTY_NOTICES.md` with its
  license, in the same pull request that adds it.
- Code taken from another project keeps its copyright line and is noted in
  `THIRD_PARTY_NOTICES.md`, even when it is three lines of an algorithm.
- A new file needs no license header; the repository-level `LICENSE` covers
  it. If you want one, the Apache boilerplate at the end of `LICENSE` is the
  text to use.
- Contributing to this repository licenses your contribution under
  Apache-2.0 (section 5 of the license).

## Third-party dependencies

All of them are fetched at build time, none is vendored here, and every one
is permissive:

| Component | License | Used by |
|---|---|---|
| slang | MIT | every tool that reads SystemVerilog |
| {fmt}, tomlplusplus | MIT | pulled in by slang |
| Boost.Regex | Boost Software License 1.0 | pulled in by slang |
| libfst | MIT | FST waveforms |
| CaDiCaL | MIT | `mformal`, `mequiv`, `msynth` |
| OpenSSL | Apache-2.0 | `msimprotect`, where the system provides it |
| zlib | zlib | FST compression |

`THIRD_PARTY_NOTICES.md` carries the license texts themselves.
