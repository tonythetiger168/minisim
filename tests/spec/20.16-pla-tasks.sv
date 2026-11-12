// IEEE 1800-2023 20.16: PLA modeling tasks. The personality memory has one
// word per output term and one bit per input term. `array` format: 1 takes
// the input, 0 leaves it out. `plane` format: 1 the input, 0 its
// complement, z or ? no part. Async tasks evaluate again when an input or
// a memory word changes; sync tasks only when called.
//! icarus-skip: Icarus 13 does not implement the PLA tasks
module top;
  logic [1:7] mem [1:3];
  logic [1:3] pl [1:2];
  logic a1, a2, a3, a4, a5, a6, a7;
  logic b1, b2, b3;
  logic [1:3] o_or, o_nand, o_nor, o_sync;
  logic [1:2] p_and, p_or;
  logic [1:3] pin;
  initial begin
    mem[1] = 7'b1100000;
    mem[2] = 7'b0011100;
    mem[3] = 7'b0000111;
    pl[1] = 3'b10z;
    pl[2] = 3'b?11;
    {a1, a2, a3, a4, a5, a6, a7} = 7'b1111000;
    pin = 3'b101;
    $async$and$array(mem, {a1, a2, a3, a4, a5, a6, a7}, {b1, b2, b3});
    $async$or$array(mem, {a1, a2, a3, a4, a5, a6, a7}, o_or);
    $async$nand$array(mem, {a1, a2, a3, a4, a5, a6, a7}, o_nand);
    $async$nor$array(mem, {a1, a2, a3, a4, a5, a6, a7}, o_nor);
    $sync$and$array(mem, {a1, a2, a3, a4, a5, a6, a7}, o_sync);
    $async$and$plane(pl, pin, p_and);
    $async$or$plane(pl, pin, p_or);
    #1 $display("%b%b%b %b %b %b %b %b %b", b1, b2, b3, o_or, o_nand, o_nor, o_sync, p_and, p_or);
    a5 = 1; a6 = 1; a7 = 1; pin = 3'b011;
    #1 $display("%b%b%b %b %b %b %b %b %b", b1, b2, b3, o_or, o_nand, o_nor, o_sync, p_and, p_or);
    mem[1] = 7'b0000001; a7 = 0;
    #1 $display("%b%b%b %b %b %b %b %b %b", b1, b2, b3, o_or, o_nand, o_nor, o_sync, p_and, p_or);
    $sync$and$array(mem, {a1, a2, a3, a4, a5, a6, a7}, o_sync);
    $display("%b", o_sync);
  end
endmodule
//> 100 110 011 001 100 10 11
//> 111 111 000 000 100 01 01
//> 010 011 101 100 100 01 01
//> 010
