// msim -cm_cond modes (as VCS): std (sensitized vectors), full (every
// combination of the term values) and allops (also & and | of one-bit operands).
module tb;
  logic a = 0, b = 0, c = 0, y, z;
  always_comb y = a && b && c;
  always_comb z = (a & b) | c;
  initial begin
    #1 a = 1; #1 b = 1; #1 c = 1; #1 a = 0; #1 $finish;
  end
endmodule
