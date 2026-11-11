//! args: -only unused,connect -enable CON002 -top unu
module unu(input logic clk, a, b, unused_in, spare,    //~ UNU004
           output logic y, o_float);              //~ UNU005
  logic never;                    //~ UNU001
  logic dead;                     //~ UNU002
  logic floating;                 //~ UNU003
  logic unused_tie;               // the name says so
  logic used, q, pos_q;
  assign dead = a;
  assign used = a & floating;
  assign y = used ^ q;
  assign impl = b;                //~ CON004 UNU002
  assign unused_tie = b;
  unu_leaf u0(.d(used), .clk(clk), .q(q));
  unu_leaf u1(.d(), .clk(clk), .q());            //~ CON001 CON002
  unu_leaf u2(a, clk, pos_q);                    //~ CON003
  logic unused_q;
  assign unused_q = pos_q;
endmodule

module unu_leaf(input logic d, clk, output logic q);
  always_ff @(posedge clk) q <= d;
endmodule
