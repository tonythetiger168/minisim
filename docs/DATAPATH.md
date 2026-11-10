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
| `MW_mult_seq` | shift-and-add multiplier, one bit per clock, `start` / `done` |
| `MW_div` | quotient and remainder as the `/` and `%` operators give them, `divide_by_0` |
| `MW_div_seq` | the same, one quotient bit per clock, `start` / `done` |
| `MW_sqrt` | integer square root and remainder |
| `MW_sat` | narrow to `OUT_WIDTH` bits with clipping |

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
| `MW_decode` | binary to one-hot |
| `MW_onehot2bin` | one-hot to binary, with an error flag |
| `MW_bin2gray`, `MW_gray2bin` | Gray code conversion |
| `MW_parity` | parity generator and checker |
| `MW_mux`, `MW_mux_onehot` | `NUM`-to-1 multiplexer, binary or one-hot select |
| `MW_ecc` | Hamming SECDED: generate check bits, correct one error, flag two |

## Sequential

| Module | Function |
| --- | --- |
| `MW_counter` | up/down counter with load and terminal count |
| `MW_gray_counter` | Gray code counter |
| `MW_lfsr` | Galois LFSR |
| `MW_crc` | parallel CRC, any polynomial, `DATA_WIDTH` bits per clock, normal or reflected |
| `MW_sync` | multi-flop synchronizer |
| `MW_ram` | flip-flop memory, one write and one read port, registered or combinational read |
| `MW_fifo_sync` | single-clock FIFO with count, almost-full / almost-empty and error flags |
| `MW_fifo_async` | dual-clock FIFO with Gray code pointers |
| `MW_arb_fp`, `MW_arb_rr` | fixed priority and round-robin arbiters |

## Testing

`tests/dw/*.sv` are self-checking benches (`ctest -L dw`). Each one
instantiates the modules at several widths and in every architecture, drives
random and corner-case inputs, and compares the outputs with the
SystemVerilog operators or a behavioural model: `arith.sv`, `muldiv.sv`,
`bits.sv`, `seq.sv`. The CRC bench checks the published check values of
CRC-16/CCITT-FALSE and CRC-32. The same benches also pass on Verilator 5, and
the library is clean under `verilator --lint-only -Wall`.

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
