# BSIM4 4.8.3

The BSIM4 MOSFET model equations, written and owned by the BSIM Research
Group at the University of California, Berkeley (project directors Prof.
Sayeef Salahuddin and Prof. Chenming Hu). Copyright (c) 2025 University of
California. mspice acknowledges the UC Berkeley BSIM Research Group as the
developer of this model.

- Version: BSIM4 4.8.3, released 2025-05-19.
- License: Educational Community License 2.0, stated in the header of every
  file here; the text is in `LICENSE`.
- Taken from: the ngspice source tree, `src/spicelib/devices/bsim4`, commit
  a80f6e3e95d51534905b1f23410a951802666656.
- Files: `b4.c b4acld.c b4check.c b4geo.c b4ld.c b4mpar.c b4par.c b4set.c
  b4temp.c b4trunc.c bsim4def.h bsim4ext.h`. Only files that carry the
  ECL-2.0 header were taken (the simulator-specific binding and
  safe-operating-area files of that folder do not, and are not here).
- Changes: none. The files are byte for byte as distributed. Do not edit
  them; to update, replace them with a newer release and re-run the tests
  `spice/bsim4*`.

The files expect the SPICE3 device interface. mspice provides it in its own
code: `src/spice_bsim4/shim.h` (and the one-line headers under
`src/spice_bsim4/ngspice/` that the `#include` lines of the model files find)
and `src/spice_bsim4.cpp`.
