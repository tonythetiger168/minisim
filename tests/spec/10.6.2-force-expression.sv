// IEEE 1800-2023 10.6.2: A forced expression keeps following its operands
// until release; a released variable keeps its value, a released net goes
// back to its drivers (10.6.2).
//! icarus-skip: Icarus 13 evaluates a force expression only once
module top;
  logic a = 0, b = 0, v;
  wire w;
  assign w = a;
  initial begin
    v = 1;
    #1 force w = b; force v = b & a;
    #1 $display("%b %b", w, v);
    b = 1; a = 1;
    #1 $display("%b %b", w, v);
    v = 0;
    #1 $display("%b", v);
    release w; release v;
    a = 0;
    #1 $display("%b %b", w, v);
  end
endmodule

//> 0 0
//> 1 1
//> 1
//> 0 1
