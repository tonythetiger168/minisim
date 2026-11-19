// msim -compile leaves out a clocked process's NBA that would not change its
// register, when that NBA is the register's only write in the design
// (msim.cpp soleNbaTargets, Sim::nbaKeep; MSIM_NONBASKIP=1: off). Most
// registers keep their value on most edges: 99% of OpenTitan's NBAs.
// e has two NBAs on exclusive branches (a reset and a value): also checked.
// The cases where leaving one out would be wrong must still print the same:
//  - r: two NBAs in one process (a default, then an override),
//  - b: also written by a task (a blocking write between the edge and the NBA region),
//  - c: its process runs twice in one time slot (a clock glitch), the second
//    run's NBA equals the old value but must replace the first run's.
module nbaskip;
  reg clk = 0, clk2 = 0, d = 0;
  reg [7:0] q, r = 0, b, c, e;
  reg [3:0] cnt = 0;
  always @(posedge clk) q <= cnt[1] ? 8'd5 : 8'd0;
  always @(posedge clk) begin r <= r; if (cnt[0]) r <= r + 8'd1; end
  always @(posedge clk) b <= 8'd7;
  always @(posedge clk) if (cnt[3:1] == 3'd0) e <= 8'd1; else e <= 8'd2;
  always @(posedge clk) cnt <= cnt + 4'd1;
  always @(posedge clk2) c <= {7'd0, d};
  task poke_b; b = 8'd9; endtask
  initial begin
    repeat (6) begin #5 clk = 1; #1 poke_b; #4 clk = 0; end
    #1 clk2 = 1; #1 clk2 = 0; d = 1;
    #1 clk2 = 1; #0 clk2 = 0; d = 0; #0 clk2 = 1;
    #1 $display("q=%0d r=%0d b=%0d c=%0d e=%0d (c: want 0)", q, r, b, c, e);
    $finish;
  end
  always @(q or r or b or e) $display("%0t q=%0d r=%0d b=%0d e=%0d", $time, q, r, b, e);
endmodule
