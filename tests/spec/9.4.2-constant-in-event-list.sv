// 9.4.2: an event control may name a constant (a literal, a parameter or an
// enum name). It never changes, so it never triggers; the other events in
// the list still do.
//! icarus-skip: Icarus 13 triggers once at time 0 on a constant in the event list
module top;
  enum bit [1:0] {IDLE, RUN} st;
  parameter P = 1;
  logic m;
  int n;
  always @(m or 1'b0 or IDLE or P) n++;
  initial begin
    #1 m = 1;
    #1 m = 0;
    #1 $display("%0d", n);
  end
endmodule
//> 2
