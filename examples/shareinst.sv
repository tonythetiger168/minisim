// Four instances of one small core (registers, a decoder, clocked
// blocking temporaries, a register file written at a run-time index).
// msim -compile emits one copy of each module's code and calls it with
// each instance's signal offsets (codegen.hpp shareChunks,
// emitDomainFns); every instance must still compute its own values.
module shcore #(parameter logic [7:0] SEED = 8'h01) (input logic clk, output logic [7:0] sum);
  logic [7:0] acc = SEED, pc = 0, dec, t;
  logic [7:0] rf [0:7];
  logic [1:0] st = 0;
  always_comb begin
    case (st)
      2'd0: dec = acc + 8'd3;
      2'd1: dec = acc ^ pc;
      2'd2: dec = {acc[6:0], acc[7]};
      default: dec = ~acc;
    endcase
  end
  always @(posedge clk) begin
    t = dec;
    if (t[0]) t = t ^ 8'h5a;
    acc <= t;
    st <= st + 2'd1;
    pc <= pc + 8'd1;
    rf[pc[2:0]] <= t;
  end
  initial for (int k = 0; k < 8; k++) rf[k] = 8'(k);
  assign sum = rf[0] ^ rf[3] ^ rf[5] ^ acc;
endmodule

module shareinst;
  logic clk = 0;
  logic [7:0] s0, s1, s2, s3;
  always #5 clk = ~clk;
  shcore #(8'h01) c0 (clk, s0);
  shcore #(8'h17) c1 (clk, s1);
  shcore #(8'h2b) c2 (clk, s2);
  shcore #(8'h40) c3 (clk, s3);
  initial begin
    #1000 $display("sums=%h %h %h %h", s0, s1, s2, s3);
    $finish;
  end
endmodule
