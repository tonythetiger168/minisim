# Third-party notices

Open-source code that is linked or compiled into the minisim tools, and
open-source data kept in the tree. Each
entry names the project, where it is used, its license, and the license text.
Programs that are only run as external tools are not listed here.

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
