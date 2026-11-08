// IEEE 1800-2023 27 Generate constructs: a loop generate (27.4) with named
// blocks and per-iteration signals, a conditional generate (27.5) that keeps
// one branch, a case generate, and continuous assignments to separate bits of
// one vector from each iteration.
module adder1 (input logic a, b, ci, output logic s, co);
  assign s  = a ^ b ^ ci;
  assign co = (a & b) | (ci & (a ^ b));
endmodule

module t;
  parameter int N = 4;
  parameter bit FAST = 1;
  logic [N-1:0] a, b, s;
  wire  [N:0]   c;
  assign c[0] = 1'b0;
  for (genvar k = 0; k < N; k++) begin : bit_
    logic p;
    assign p = a[k] ^ b[k];
    adder1 u (.a(a[k]), .b(b[k]), .ci(c[k]), .s(s[k]), .co(c[k+1]));
  end
  if (FAST) begin : g_fast
    initial $display("fast");
  end else begin : g_slow
    initial $display("slow");
  end
  case (N)
    4: begin : g4  initial $display("N is 4"); end
    default: begin : gd initial $display("other N"); end
  endcase
  initial begin
    a = 4'd9; b = 4'd5;
    #1 $display("%0d carry=%b p2=%b", s, c[N], bit_[2].p);
  end
endmodule
//> fast
//> N is 4
//> 14 carry=0 p2=1
