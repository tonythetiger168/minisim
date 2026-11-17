// IEEE 1800-2023 13.5 and 6.12.1: an inout argument is copied in as if
// assigned to the formal, and copied out as if assigned to the actual, so a
// real actual of an integral formal is rounded going in and converted back
// coming out, and an integral actual of a real formal becomes a real.
module top;
  real r;
  int i;
  task add_u(inout bit [7:0] io, input bit [7:0] d); io = io + d; endtask
  task automatic add_s(inout bit signed [7:0] io, input bit signed [7:0] d); io = io + d; endtask
  task half(inout real x); x = x / 2; endtask
  initial begin
    r = 5.0;   add_u(r, 7);  $display("%0.1f", r);
    r = -3.0;  add_s(r, -7); $display("%0.1f", r);
    r = 2.6;   add_u(r, 1);  $display("%0.1f", r);
    i = -9;    half(i);      $display("%0d", i);
  end
endmodule
//> 12.0
//> -10.0
//> 4.0
//> -5
