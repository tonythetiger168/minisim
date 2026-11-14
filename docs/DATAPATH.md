# Datapath library (`dw/`)

`dw/` holds synthesizable, parameterized SystemVerilog building blocks, one
module per file, all named `MW_<function>`. They are plain RTL with no vendor
cells, so they simulate in any IEEE 1800 simulator and go through any
synthesis tool.

```sh
msim tb.sv rtl/*.sv -y dw +libext+.sv -top tb     # modules are found by name
```

Conventions:

- Widths are parameters (`WIDTH`, or `A_WIDTH` / `B_WIDTH`). Nothing is fixed.
- `tc` is an input, not a parameter: 0 treats the operands as unsigned, 1 as
  two's complement, so one instance serves both.
- Several inputs of the same width come packed in one vector,
  `{in[NUM-1], ..., in[0]}`.
- Sequential blocks use `clk` and an asynchronous active-low `rst_n`.
- `ARCH` picks an implementation. Every architecture of a module gives the
  same outputs; they differ in area and delay.
- An illegal parameter stops elaboration with `$error`.

## Families

Every family of a commercial building-block library (Synopsys DesignWare
Building Blocks, the Cadence ChipWare equivalents) and what `dw/` has for it.
Members of a family that `dw/` does not have yet are named in the last column.

| Family | `dw/` modules | Not in `dw/` |
| --- | --- | --- |
| Arithmetic: add, subtract, compare, sum | `MW_add` (5 architectures), `MW_sub`, `MW_addsub`, `MW_inc`, `MW_dec`, `MW_incdec`, `MW_addsat`, `MW_absval`, `MW_cmp`, `MW_minmax`, `MW_csa`, `MW_csa_tree`, `MW_sum`, `MW_sat` | |
| Arithmetic: multiply, divide, square root | `MW_mult` (2 architectures), `MW_square`, `MW_mac`, `MW_prod_sum`, `MW_div`, `MW_sqrt` | |
| Pipelined arithmetic (for retiming) | `MW_mult_pipe`, `MW_div_pipe`, `MW_sqrt_pipe` | a pipelined sum of products |
| Sequential (multi-cycle) arithmetic | `MW_mult_seq`, `MW_div_seq` | a multi-cycle square root |
| Transcendental and trigonometric | `MW_log2`, `MW_exp2`, `MW_sincos` | |
| Floating point (IEEE 754) | `MW_fp_add`, `MW_fp_sub`, `MW_fp_addsub`, `MW_fp_mult`, `MW_fp_div`, `MW_fp_sqrt`, `MW_fp_mac`, `MW_fp_recip`, `MW_fp_flt2flt`, `MW_fp_cmp`, `MW_fp_i2flt`, `MW_fp_flt2i` | floating point log, exp, sin/cos |
| Combinational logic: shift, count, encode, select | `MW_shifter`, `MW_rotate`, `MW_lzd`, `MW_lsd`, `MW_norm`, `MW_popcount`, `MW_pri_enc`, `MW_thermdec`, `MW_decode`, `MW_onehot2bin`, `MW_bin2gray`, `MW_gray2bin`, `MW_mux`, `MW_mux_onehot` | |
| Sequential logic: counters, LFSR | `MW_counter`, `MW_gray_counter`, `MW_lfsr` | |
| Data integrity: CRC, ECC, parity | `MW_crc`, `MW_ecc`, `MW_parity` | |
| Line coding | `MW_8b10b_enc`, `MW_8b10b_dec` | |
| Memory: FIFO, stack, RAM | `MW_fifo_sync`, `MW_fifo_async`, `MW_stack`, `MW_ram` | separate FIFO controllers for an external RAM |
| Clock domain crossing | `MW_sync`, `MW_reset_sync`, `MW_pulse_sync`, `MW_data_sync`, `MW_fifo_async` | |
| DSP | `MW_fir` | an IIR filter |
| Test: JTAG | `MW_tap` | boundary scan cells |
| Control: arbiters | `MW_arb_fp`, `MW_arb_rr` | |
| Generic gates (GTECH) | not applicable: generic gates are internal to a synthesis tool; msynth maps to its own generic cells and then to a Liberty library (mini45 or sky130) | |

## Arithmetic

| Module | Function |
| --- | --- |
| `MW_add` | `sum = a + b + ci`, carry out. `ARCH`: 0 ripple, 1 carry lookahead (4-bit blocks), 2 Kogge-Stone, 3 Brent-Kung, 4 carry select |
| `MW_sub` | `diff = a - b - bi`, borrow out |
| `MW_addsub` | add or subtract, chosen by `add_sub` |
| `MW_inc`, `MW_dec`, `MW_incdec` | `a + 1`, `a - 1`, either |
| `MW_addsat` | saturating add / subtract, signed or unsigned |
| `MW_absval` | absolute value |
| `MW_cmp` | `lt gt eq le ge ne`, signed or unsigned |
| `MW_minmax` | minimum or maximum of `NUM` inputs, with its index |
| `MW_csa` | carry save adder (3:2 compressor row) |
| `MW_csa_tree` | Wallace tree: `NUM` addends to two |
| `MW_sum` | sum of `NUM` inputs (Wallace tree and one adder) |
| `MW_mult` | `a * b`. `ARCH`: 0 array, 1 radix-4 Booth with a Wallace tree and a prefix adder |
| `MW_square` | `a * a` |
| `MW_mac` | `a * b + c`, the addend folded into the tree |
| `MW_mult_pipe` | multiplier with `STAGES` register stages (for retiming) |
| `MW_div_pipe`, `MW_sqrt_pipe` | divider and square root with `STAGES` register stages |
| `MW_prod_sum` | sum of `NUM` products (dot product), unsigned or two's complement |
| `MW_log2` | `log2(a)` of a normalized 1.x fixed-point number, a 0.x result under one unit low |
| `MW_exp2` | `2^a` of a 0.x fraction, a 1.x result under one unit low |
| `MW_mult_seq` | shift-and-add multiplier, one bit per clock, `start` / `done` |
| `MW_div` | quotient and remainder as the `/` and `%` operators give them, `divide_by_0` |
| `MW_div_seq` | the same, one quotient bit per clock, `start` / `done` |
| `MW_sqrt` | integer square root and remainder |
| `MW_sat` | narrow to `OUT_WIDTH` bits with clipping |
| `MW_sincos` | sine and cosine of an angle in turns (CORDIC), within 2 units of `$sin`/`$cos` |

## Shifting, counting and coding

| Module | Function |
| --- | --- |
| `MW_shifter` | barrel shifter: left, logical right, arithmetic right |
| `MW_rotate` | barrel rotator, any width |
| `MW_lzd` | leading zero count |
| `MW_lsd` | leading sign bit count |
| `MW_norm` | shift left until the top bit is 1 |
| `MW_popcount` | number of 1 bits |
| `MW_pri_enc` | priority encoder, lowest or highest bit first |
| `MW_thermdec` | binary to thermometer code |
| `MW_decode` | binary to one-hot |
| `MW_onehot2bin` | one-hot to binary, with an error flag |
| `MW_bin2gray`, `MW_gray2bin` | Gray code conversion |
| `MW_parity` | parity generator and checker |
| `MW_mux`, `MW_mux_onehot` | `NUM`-to-1 multiplexer, binary or one-hot select |
| `MW_ecc` | Hamming SECDED: generate check bits, correct one error, flag two |
| `MW_8b10b_enc`, `MW_8b10b_dec` | IEEE 802.3 clause 36 8b/10b line code with running disparity; the decoder flags codes no character has and disparity errors |

## Floating point

IEEE 754 binary arithmetic in any format: operands and results are
`{sign, exponent[EXP-1:0], fraction[FRAC-1:0]}`, with `EXP` and `FRAC`
parameters (half 5/10, bfloat16 8/7, single 8/23, double 11/52, or anything
else). Subnormals are full IEEE, not flushed to zero, and every result is
rounded once, correctly.

- `rnd[2:0]`: 0 to nearest, ties to even; 1 toward zero; 2 toward +inf;
  3 toward -inf; 4 to nearest, ties away from zero.
- `status[4:0]` = `{inexact, underflow, overflow, divzero, invalid}`.
  Underflow is tininess before rounding together with inexact.
- A NaN result is the canonical quiet NaN (sign 0, fraction MSB 1, rest 0).
  A signaling NaN operand, `inf - inf`, `0 * inf`, `0/0`, `inf/inf` and the
  square root of a negative number raise invalid.

| Module | Function |
| --- | --- |
| `MW_fp_add`, `MW_fp_sub` | `z = a + b`, `z = a - b` |
| `MW_fp_addsub` | add or subtract, chosen by `op` (1 subtracts) |
| `MW_fp_mult` | `z = a * b` |
| `MW_fp_div` | `z = a / b`; finite / 0 gives a signed infinity and divzero |
| `MW_fp_sqrt` | `z = sqrt(a)`; `sqrt(-0) = -0` |
| `MW_fp_mac` | fused `z = a * b + c`, one rounding; `0 * inf` is invalid even with a quiet NaN `c` |
| `MW_fp_recip` | `z = 1 / a` |
| `MW_fp_flt2flt` | conversion between two formats (`EXP_A`/`FRAC_A` to `EXP_Z`/`FRAC_Z`), narrowing or widening |
| `MW_fp_cmp` | `altb`, `aeqb`, `agtb`, `unordered`; `z0`, `z1` are IEEE 754-2019 minimumNumber and maximumNumber (swapped when `zctr` = 1), with `-0 < +0` |
| `MW_fp_i2flt` | `IW`-bit integer to float, unsigned or two's complement (`tc`) |
| `MW_fp_flt2i` | float to `IW`-bit integer by `rnd`; NaN, infinity and out-of-range values saturate and raise invalid |
| `MW_fp_round` | the shared step: rounds `sig * 2^e` (plus a sticky bit) into the format |

Division and square root reuse `MW_div` and `MW_sqrt` on the significands.

## Sequential

| Module | Function |
| --- | --- |
| `MW_counter` | up/down counter with load and terminal count |
| `MW_gray_counter` | Gray code counter |
| `MW_lfsr` | Galois LFSR |
| `MW_crc` | parallel CRC, any polynomial, `DATA_WIDTH` bits per clock, normal or reflected |
| `MW_sync` | multi-flop synchronizer |
| `MW_reset_sync` | reset synchronizer: asserts at once, deasserts `STAGES` clocks after release |
| `MW_pulse_sync` | single-clock pulse into another clock domain (toggle handshake), with `busy` |
| `MW_data_sync` | a `WIDTH`-bit word into another clock domain (four-phase handshake), with `busy` and `data_avail` |
| `MW_fir` | transposed-form FIR filter, `TAPS` signed coefficients from an input vector, full-precision output |
| `MW_tap` | IEEE 1149.1 TAP controller with IR, BYPASS and IDCODE registers and strobes for user data registers |
| `MW_ram` | flip-flop memory, one write and one read port, registered or combinational read |
| `MW_fifo_sync` | single-clock FIFO with count, almost-full / almost-empty and error flags |
| `MW_fifo_async` | dual-clock FIFO with Gray code pointers |
| `MW_stack` | LIFO stack with count, full / empty and error flags |
| `MW_arb_fp`, `MW_arb_rr` | fixed priority and round-robin arbiters |

## Testing

`tests/dw/*.sv` are self-checking benches (`ctest -L dw`). Each one
instantiates the modules at several widths and in every architecture, drives
random and corner-case inputs, and compares the outputs with the
SystemVerilog operators or a behavioural model: `arith.sv`, `muldiv.sv`,
`bits.sv`, `seq.sv`, `more.sv`, `xing.sv`. The 8b/10b bench round-trips all
268 characters at both disparities and checks every 10-bit code at the decoder. The CRC bench checks the published check values of
CRC-16/CCITT-FALSE and CRC-32. The same benches also pass on Verilator 5, and
the library is clean under `verilator --lint-only -Wall`.

The floating point modules have their own check, `tests/dw_fp_test.py`
(`dw/fp` in ctest): an exact reference written with Python fractions rounds
each result once, and msim must match it bit for bit, flags included, in five
formats (4/3, half, bfloat16, single, double) and all five rounding modes;
the fused multiply-add gets operands that nearly cancel, and the format
conversion goes from each format to another, wider or narrower.
A third of the operands are zeros, infinities, quiet and signaling NaNs,
subnormals, the largest and smallest numbers, and near-cancelling pairs.
`dw_fp_test.py REPO MSIM 3000` runs 75000 vectors instead of 7500.

## Choosing an adder

Area and delay grow differently with width, so the best `ARCH` depends on
the timing budget:

| `ARCH` | Logic levels at width n | Area | Use |
| --- | --- | --- | --- |
| 0 ripple | n | smallest | slack to spare, or narrow |
| 1 carry lookahead | about n/4 + 3 | small | mid-size, moderate timing |
| 4 carry select | about n/BLOCK + BLOCK | about 2x ripple | moderate timing |
| 3 Brent-Kung | 2 log2(n) - 1 | low for a prefix adder | wide and fast, wiring matters |
| 2 Kogge-Stone | log2(n) | n log2(n) cells | the critical path |
