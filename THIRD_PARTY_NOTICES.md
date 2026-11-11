# Third-party notices

Open-source code that is linked or compiled into the minisim tools. Each
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
