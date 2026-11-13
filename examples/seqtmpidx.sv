// A clocked block that keeps working values in blocking temporaries
// (msim.cpp: localizeSeqBlocking), with run-time index writes into them
// (w[ci] = b, w[k +: 4] = n), ahead of a testbench whose stimulus comes from
// $urandom. msim's static schedule must print the same as the event-driven
// reference: the same bits written, and the same random stimulus
// (tests/msim_seqtmp.cmake; tests/msim_compile.cmake with +define+LFSR).
module stcore(input logic clk, rst_n, input logic [7:0] din, output logic [31:0] acc);
  logic [5:0] row;
  logic [47:15] rw;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin row <= '0; rw <= '0; acc <= '0; end
    else begin : b_main
      logic       b;
      logic [5:0] ci;
      logic [47:0] w;
      logic [2:0] k;
      b = ^din;
      ci = 6'd47 - row;
      w = {rw, 15'd0};
      w[ci] = b;
      k = din[2:0];
      w[{k, 2'b00} +: 4] = din[7:4];
      rw <= w[47:15];
      row <= row == 6'd47 ? 6'd0 : row + 6'd1;
      acc <= acc * 33 ^ 32'(w[47:15]) ^ 32'(ci);
    end
endmodule

module seqtmpidx;
  logic clk = 0, rst_n = 0;
  always #5 clk = ~clk;
  logic [7:0] din = 0;
  wire [31:0] a0, a1;
  stcore u0(clk, rst_n, din, a0);
  stcore u1(clk, rst_n, ~din, a1);
`ifdef LFSR   // msim -compile: its $urandom is one stream, not one per process
  always @(negedge clk) din = {din[6:0], din[7] ^ din[5] ^ din[4] ^ din[3] ^ (din == 0)};
  initial begin
    #12 rst_n = 1;
    #4000 $display("a0=%h a1=%h din=%h", a0, a1, din);
    $finish;
  end
`else
  always @(negedge clk) din = $urandom;
  initial begin
    #12 rst_n = 1;
    #4000 $display("a0=%h a1=%h din=%h r=%h", a0, a1, din, $urandom);
    $finish;
  end
`endif
endmodule
