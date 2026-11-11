# Third-party notices

Software that minisim's tools use but that is not part of this repository's
own code. Each entry says how it is used and under which licence.

## ABC

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
