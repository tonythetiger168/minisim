`timescale 1ns/1ns
// IEEE 1800-2023 31.5, 31.7, 31.8: edge-control specifiers, conditioned
// events (=== is deterministic: x does not enable; == is not: x enables),
// and a vector terminal, where a change of several bits is one transition.
//! icarus-skip: Icarus 13 does not run timing checks
module m(input clk, input en, input [3:0] bus);
  specify
    $setup(bus, posedge clk &&& en, 3);
    $setup(bus, posedge clk &&& (en == 1'b1), 3);
    $setup(bus, posedge clk &&& (en === 1'b1), 3);
    $setup(bus, edge[01] clk, 3);
  endspecify
endmodule
module top;
  reg clk = 0, en = 1;
  reg [3:0] bus = 0;
  m u(clk, en, bus);
  initial begin
    #8 bus = 4'b0110;   // two bits at once
    #2 clk = 1;         // 10: en is 1
    #5 clk = 0;
    en = 1'bx;
    #3 bus = 4'b1111;
    #2 clk = 1;         // 20: en is x
    #5 clk = 0;
    en = 0;
    #3 bus = 0;
    #2 clk = 1'bx;      // 30: 0 to x is not edge[01], and en is 0
    #10 $finish;
  end
endmodule

//> "31.7-conditions-edges.sv", 8: Timing violation in top.u
//>     $setup( bus:8, posedge clk &&& en:10, 3 );
//> "31.7-conditions-edges.sv", 9: Timing violation in top.u
//>     $setup( bus:8, posedge clk &&& (en == 1'b1):10, 3 );
//> "31.7-conditions-edges.sv", 10: Timing violation in top.u
//>     $setup( bus:8, posedge clk &&& (en === 1'b1):10, 3 );
//> "31.7-conditions-edges.sv", 11: Timing violation in top.u
//>     $setup( bus:8, edge[01] clk:10, 3 );
//> "31.7-conditions-edges.sv", 9: Timing violation in top.u
//>     $setup( bus:18, posedge clk &&& (en == 1'b1):20, 3 );
//> "31.7-conditions-edges.sv", 11: Timing violation in top.u
//>     $setup( bus:18, edge[01] clk:20, 3 );
