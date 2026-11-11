# Third-party notices

Open-source code that is linked or compiled into the minisim tools, and
open-source data kept in the tree. Each
entry names the project, where it is used, its license, and the license text.
Programs that are only run as external tools are not linked or compiled in;
the ones CI builds from source are listed at the end.

## slang and slang-tidy

- Project: https://github.com/MikePopoloski/slang (v12.1)
- Used by: the SystemVerilog front end of `msim`, `mlint`, `mdft` and the
  other tools that read RTL; `mlint` also compiles in the checks of
  slang-tidy (`tools/tidy` in the slang tree) as its `TIDY` rules
  (`src/linttidy.hpp`, `cmake/tools/mlint_tidy.cmake`).
- License: MIT

```
Copyright (c) 2015-2026 Michael Popoloski

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## BSIM4 4.8.3

- Project: the BSIM4 MOSFET model of the UC Berkeley BSIM Research Group
  (https://bsim.berkeley.edu/models/bsim4/), files as distributed in the
  ngspice source tree (commit a80f6e3e95d51534905b1f23410a951802666656).
- Used by: `mspice`, MOSFET model level 14/54. The files are in
  `third_party/bsim4/`, unmodified; build option `MINISIM_WITH_BSIM4`
  (`cmake/tools/mspice.cmake`).
- Copyright (c) 2025 University of California. Project directors: Prof.
  Sayeef Salahuddin and Prof. Chenming Hu. Developers:
  https://www.bsim.berkeley.edu/models/bsim4/auth_bsim4/
- Acknowledgment: the BSIM4 model was developed by the UC Berkeley BSIM
  Research Group. It is supported by the members of Silicon Integration
  Initiative's Compact Model Coalition (http://www.si2.org/cmc).
- License: Educational Community License, Version 2.0
  (http://opensource.org/licenses/ECL-2.0). The full text is in
  `third_party/bsim4/LICENSE` and is installed as `LICENSE.bsim4`.

## SkyWater sky130 cell library (test data)

- Project: the SkyWater Open Source PDK, library `sky130_fd_sc_hd`
  (https://github.com/google/skywater-pdk). Liberty from
  https://github.com/efabless/skywater-pdk-libs-sky130_fd_sc_hd at commit
  `5a42fd01a0eeb2338340438443c7062e198d97a4`; LEF and technology LEF from
  https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts at commit
  `bfcc1895a7a1d9ff6e677066825812e549b72421`.
- Used by: `lib/sky130` holds unmodified extracts (47 cells, 51 macros, the
  technology LEF) as a second test library; the `sky130` test runs the
  tools on them. Not compiled or linked into any tool. See
  `lib/sky130/README.md`.
- License: Apache-2.0, Copyright 2020 The SkyWater PDK Authors. The license
  text is `lib/sky130/LICENSE`; the upstream repositories carry no NOTICE
  file.

## Rule sets followed without their code

- Verible (https://github.com/chipsalliance/verible, Apache-2.0): `mlint`'s
  text and syntax rules (`src/lintsrc.hpp`) check the same things as a
  number of Verible's lint rules and name them in `mlint -explain`. They
  are written for slang's syntax tree; no Verible source is included.

## Programs run as external tools

Not linked into any minisim binary, and no source of theirs is in this
repository.

### ABC

- What: ABC, System for Sequential Synthesis and Verification
  (https://github.com/berkeley-abc/abc), The Regents of the University of
  California.
- Use: `msynth` runs it as a separate program for logic restructuring at
  high effort (`src/synth/abc.hpp`, `docs/SYNTH.md`). It is not linked into
  any minisim binary and no ABC source is in this repository. CI builds it
  from source at a pinned commit; a user installs it or gets it with Yosys
  (`yosys-abc`).
- Licence: permissive, BSD-style. From ABC's `copyright.txt`:

  > Copyright (c) The Regents of the University of California. All rights
  > reserved.
  >
  > Permission is hereby granted, without written agreement and without
  > license or royalty fees, to use, copy, modify, and distribute this
  > software and its documentation for any purpose, provided that the above
  > copyright notice and the following two paragraphs appear in all copies
  > of this software.
  >
  > IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY
  > FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
  > ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF
  > THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
  > DAMAGE.
  >
  > THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,
  > INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
  > AND FITNESS FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS
  > ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION
  > TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
