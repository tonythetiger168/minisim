// msim -compile: a register a clocked process writes that nothing live
// reads (picorv32's dbg_ascii_instr / cached_ascii_instr) and the @* block
// around it run only while a waveform or $monitor watches (S.observed).
// u0 keeps its debug registers to itself; u1 shows the same ones on a
// port the testbench prints, so there they are live. Run with -vcd too.
module dsunit #(parameter [15:0] SEED = 1) (input clk, output [15:0] st, output [63:0] dbg);
  reg [15:0] state = SEED;
  reg [63:0] dbg_q, dbg_cached, dbg_name, dbg_cur;
  reg [7:0] dbg_cnt;
  always @* begin
    dbg_name = "idle";
    if (state[0]) dbg_name = "odd";
    if (state[3:0] == 4'd5) dbg_name = "five";
  end
  always @(posedge clk) begin
    state <= state * 16'd3 + 16'd1;
    dbg_q <= dbg_cur;
    if (state[1]) dbg_cached <= dbg_name;
    dbg_cnt <= state[7:0] ^ 8'h5a;
  end
  always @* begin
    dbg_cur = dbg_q;
    if (state[2]) dbg_cur = dbg_cached ^ {56'd0, dbg_cnt};
  end
  assign st = state;
  assign dbg = dbg_cur;
endmodule
module deadseq;
  reg clk = 0;
  wire [15:0] s0, s1;
  wire [63:0] d0, d1;
  dsunit #(.SEED(1)) u0(.clk(clk), .st(s0), .dbg(d0));
  dsunit #(.SEED(7)) u1(.clk(clk), .st(s1), .dbg(d1));
  always #1 clk = ~clk;
  integer k;
  initial begin
    for (k = 0; k < 6; k = k + 1) begin
      #9 $display("t=%0t s0=%h s1=%h d1=%h", $time, s0, s1, d1);
    end
    $finish;
  end
endmodule
