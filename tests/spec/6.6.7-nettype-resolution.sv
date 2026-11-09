// IEEE 1800-2023 6.6.7: a nettype with a resolution function. The function
// takes a dynamic array of the driver values and gives the net's value; it
// runs whenever a driver changes, and at least once at time 0, even for a
// net with no drivers.
//! icarus-skip: Icarus 13 does not support nettype
typedef logic [3:0] T;
function automatic T orAll(input T d[]);
  orAll = 0;
  foreach (d[i]) orAll |= d[i];
endfunction
function automatic real rsum(input real driver[]);
  rsum = 0.0;
  foreach (driver[i]) rsum += driver[i];
endfunction
function automatic T countDrivers(input T d[]);
  return d.size() + 4'd8;
endfunction
nettype T wor4 with orAll;
nettype real wrsum with rsum;
nettype T wcnt with countDrivers;
module drv(output wor4 o);
  assign o = 4'h8;
endmodule
module top;
  wor4 n;
  logic [3:0] a = 4'h1;
  assign n = a;
  assign n = 4'h2;
  drv u(.o(n));
  wrsum s;
  assign s = 1.5;
  assign s = 2.25;
  wcnt none;
  initial begin
    #1 $display("%h %0.2f %h", n, s, none);
    a = 4'h4;
    #1 $display("%h", n);
  end
endmodule
//> b 3.75 8
//> e
