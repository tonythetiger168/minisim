// IEEE 1800-2023 38.16 and 38.35: vpi_get_value_array and
// vpi_put_value_array read and write several elements of an array in one
// call, starting at an index: as integers (in minisim's buffer or the
// caller's, vpiUserAllocFlag), as four-state vectors, and as raw bytes.
// vpiOneValue writes one value to every element asked for. A write wakes
// the processes that wait on the element unless vpiPropagateOff is given.
//! icarus-skip: Icarus 13 has no vpi_get_value_array
//! args: data/vpi/valuearray.c
module top;
  reg [11:0] mem [0:5];
  reg signed [7:0] tiny [3:0];
  integer i;
  always @(mem[4]) $display("mem[4] changed to %h", mem[4]);
  initial begin
    for (i = 0; i < 6; i = i + 1) mem[i] = 12'h100 + i;
    mem[3] = 12'bxxxx_zzzz_1010;
    tiny[3] = -1; tiny[2] = -2; tiny[1] = 3; tiny[0] = 4;
    #1 $arrays(mem, tiny);
    #1 for (i = 0; i < 6; i = i + 1) $display("mem[%0d] = %h", i, mem[i]);
    for (i = 3; i >= 0; i = i - 1) $display("tiny[%0d] = %0d", i, tiny[i]);
  end
endmodule
//> mem[4] changed to 104
//> ints from 1: 257 258 10 260
//> user buffer from 4: 260 261 256
//> vectors from 2: 102/000 f0a/ff0
//> raw two-state from 0: 00 01 01 01
//> raw four-state from 3: 0a 0f f0 0f
//> longs of tiny from 3: -1 -2 3 4
//> mem[4] changed to 0aa
//> mem[0] = 777
//> mem[1] = 777
//> mem[2] = 102
//> mem[3] = xza
//> mem[4] = 0aa
//> mem[5] = 0bb
//> tiny[3] = -5
//> tiny[2] = 6
//> tiny[1] = 3
//> tiny[0] = 4
