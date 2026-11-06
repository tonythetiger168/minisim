// Signed arithmetic against Icarus Verilog 13.
// Same bits, signed vs unsigned compare. Sign-extend on add/sub/mul.
// Either operand unsigned makes the expression unsigned.
// >>> sign-fills. >> on a signed value zero-fills after the widen.
// $signed / $unsigned change the tag, not the bits.
module signed_arith;
  logic signed [3:0] s4;
  logic [3:0] u4;
  logic signed [7:0] s8;
  logic [7:0] u8;
  initial begin
    s4 = 4'sb1000;
    u4 = 4'b1000;
    $display("CMP_S %b", s4 < 1);
    $display("CMP_U %b", u4 < 1);
    $display("CMP_SU %b", s4 < u4);
    $display("CMP_SS %b", s4 < 4'sd1);
    $display("CMP_LE %b", s4 <= 4'sb1000);
    $display("CMP_GT %b", s4 > 4'sd1);
    $display("EQ_S %b", 4'sb1111 == 1'sb1);
    $display("EQ_M %b", 4'sb1111 == 1'b1);
    s8 = s4;
    $display("EXT_S %b", s8);
    u8 = s4;
    $display("EXT_US %b", u8);
    s8 = u4;
    $display("EXT_SU %b", s8);
    s8 = s4 + 4'sb1001;
    $display("ADD_SS %b", s8);
    u8 = s4 + 4'sb1001;
    $display("ADD_SS_U %b", u8);
    u8 = s4 + u4;
    $display("ADD_MIX %b", u8);
    s8 = s4 + u4;
    $display("ADD_MIX_S %b", s8);
    s4 = 4'sb1000;
    s8 = s4 - 4'sd1;
    $display("SUB %b", s8);
    s4 = 4'sb1000;
    s8 = s4 * 4'sd2;
    $display("MUL %b", s8);
    s4 = 4'sb1000;
    s8 = s4 >>> 1;
    $display("ASR %b", s8);
    s4 = 4'sb1000;
    s4 = s4 >>> 1;
    $display("ASR4 %b", s4);
    s4 = 4'sb1000;
    s8 = s4 >> 1;
    $display("LSR %b", s8);
    u4 = 4'b1000;
    u8 = u4 >>> 1;
    $display("ASR_U %b", u8);
    s4 = 4'sb1000;
    s8 = s4 << 1;
    $display("SHL %b", s8);
    u4 = 4'b1000;
    u8 = u4 << 1;
    $display("SHL_U %b", u8);
    u4 = 4'b1000;
    $display("CAST_S %b", $signed(u4) < 1);
    s4 = 4'sb1000;
    $display("CAST_U %b", $unsigned(s4) < 1);
    u8 = $signed(4'b1000);
    $display("CAST_EXT %b", u8);
    s8 = $unsigned(4'sb1000);
    $display("CAST_UEXT %b", s8);
    s4 = 4'sb100x;
    $display("LT_X %b", s4 < 1);
    s4 = 4'sb1000;
    s8 = s4 + 4'sbx;
    $display("ADD_X %b", s8);
    s4 = 4'sbx000;
    s8 = s4 >>> 1;
    $display("ASR_X %b", s8);
    s4 = 4'sbz000;
    s8 = s4 >>> 1;
    $display("ASR_Z %b", s8);
    s4 = 4'sb100z;
    $display("LT_Z %b", s4 < 1);
    s4 = 4'sb1000;
    s8 = s4 >>> 4;
    $display("ASR_GE %b", s8);
    s4 = 4'sb1000;
    s8 = s4 >>> 4'bx;
    $display("ASR_XC %b", s8);
    $finish;
  end
endmodule
