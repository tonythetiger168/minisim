// IEEE 1800-2023 14.3, 14.4: skews given as an edge of the clock. An output
// with `negedge` is driven at the next falling edge after the clocking event
// (plus a delay when one follows the edge); an input with `negedge` holds the
// value the signal had at the last falling edge before the event.
//! icarus-skip: Icarus 13 has no clocking blocks
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic [7:0] d = 0, e = 0, q = 8'h00;
  clocking cb @(posedge clk);
    default input #1step output negedge;
    output d;
    output negedge #2 e;
    input negedge q;
  endclocking
  always @(d or e) $display("%0t d=%h e=%h", $time, d, e);
  initial begin
    #3 q = 8'h0a;   // before the falling edge at 10
    #9 q = 8'h0b;   // at 12, after it
    @(cb) $display("%0t q=%h", $time, cb.q);
    cb.d <= 8'h11;
    cb.e <= 8'h22;
    @(cb) $display("%0t q=%h", $time, cb.q);
    #30 $finish;
  end
endmodule

//> 15 q=0a
//> 20 d=11 e=00
//> 22 d=11 e=22
//> 25 q=0b
