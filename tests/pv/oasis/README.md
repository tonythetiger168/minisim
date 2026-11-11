OASIS test layouts for `mpv` (src/oasis.hpp).

- `rndN.oas` are random files written by `gen.py <seed> <file>`; between them
  they use every record, repetition, point-list and trapezoid type, strict and
  non-strict name tables, and compressed blocks. `rndN.txt` is what KLayout
  reads from the file, as printed by `dump_klayout.py <file>` and sorted;
  test_pv prints the same from the reader and compares.
- `sky130_fd_sc_hd__*.oas` are the SKY130 cells of `../sky130` (see the
  notice there) converted to OASIS by KLayout: `inv_1` without compression,
  `dfxtp_1` with compressed blocks.
