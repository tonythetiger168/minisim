// IEEE 1800-2023 12.4.2 and 12.5.3: unique, unique0 and priority report a
// violation when no item or condition matches (unique, priority, with no
// default or else), and when more than one matches (unique, unique0). A
// casez selector with z bits can match labels that do not overlap. Reports
// come in the Observed region (12.4.2.1): after the time step's $display
// lines, not at all for a glitch that an always_comb re-run clears, and not
// while $assertcontrol turns unique checks off.
//! icarus-skip: Icarus 13 does not report unique/priority violations
module top;
  logic [1:0] s;
  logic [1:0] x = 0, y = 0, g, o;
  always_comb g = x ^ y;
  always_comb unique case (g) 0: o = 0; 1: o = 1; endcase
  initial #5 x = 2;
  initial #5 y = 2;
  initial begin
    #6 $display("glitch %0d", o);
    $assertcontrol(4, 32);   // Off for unique (20.11)
    s = 3;
    unique case (s) 0: $display("0"); endcase
    #1 $assertcontrol(3, 32);
    unique case (s) 0: $display("0"); endcase
  end
  initial begin
    s = 3;
    unique case (s) 0: $display("0"); 1: $display("1"); endcase
    unique0 case (s) 0: $display("0"); endcase
    s = 1;
    unique case (s) 1: $display("a"); 1: $display("b"); endcase
    priority case (s) 1: $display("c"); 1: $display("d"); endcase
    priority if (s == 2) $display("p");
    unique if (s == 1) $display("u1"); else if (s < 3) $display("u2");
    unique0 if (s == 2) $display("x");
    unique if (s == 1) $display("u3"); else $display("u4");
    s = 2'b0z;
    unique casez (s) 2'b00: $display("z0"); 2'b01: $display("z1"); endcase
    s = 2'b01;
    unique casez (s) 2'b00: $display("y0"); 2'b01: $display("y1"); endcase
  end
endmodule
//> a
//> c
//> u1
//> u3
//> z0
//> y1
//> Warning: "12.4.2-unique-priority-violations.sv", 26: unique case: no case item matches at time 0
//> Warning: "12.4.2-unique-priority-violations.sv", 29: unique case: more than one case item matches at time 0
//> Warning: "12.4.2-unique-priority-violations.sv", 31: priority if: no condition is true at time 0
//> Warning: "12.4.2-unique-priority-violations.sv", 32: unique if: more than one condition is true at time 0
//> Warning: "12.4.2-unique-priority-violations.sv", 36: unique case: more than one case item matches at time 0
//> glitch 0
//> Warning: "12.4.2-unique-priority-violations.sv", 22: unique case: no case item matches at time 7
