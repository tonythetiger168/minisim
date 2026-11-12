// msim -xprop: unique / priority if, casex, nested case, negedge, a for loop
// with break, a task call; constructs that are not merged (a break out of a
// while loop, a task with a nonblocking assignment) keep the standard behavior.
//! args: -xprop
module top;
  logic a, b; logic [3:0] y, z, w, u, k, s; logic [2:0] sel; logic [3:0] idx;
  logic [7:0] req; logic [1:0] st; logic clk; logic [3:0] f1, f2, f3; int n;
  logic [3:0] m, f4;
  task automatic bump(inout logic [3:0] x); x = x + 1; endtask
  task automatic later(input logic [3:0] v); f4 <= v; endtask
  always_comb begin   // unique if
    unique if (a) y = 4'd1;
    else if (b) y = 4'd3;
    else y = 4'd7;
  end
  always_comb begin   // priority if, no else
    z = 4'd0;
    priority if (a) z = 4'd5;
    else if (!a) z = 4'd6;
  end
  always_comb begin   // casex
    casex (sel)
      3'b1xx: w = 4'd8;
      3'b01x: w = 4'd4;
      default: w = 4'd0;
    endcase
  end
  always_comb begin   // a for loop with break
    idx = 4'hf;
    for (int i = 0; i < 8; i++)
      if (req[i]) begin idx = i[3:0]; break; end
  end
  always_comb begin   // a task call; an if around a case
    u = 4'd2;
    if (a) bump(u);
    k = 4'd0;
    if (b) begin
      case (st)
        2'd0: k = 4'd1;
        2'd1: k = 4'd2;
        default: k = 4'd3;
      endcase
    end
  end
  always_comb begin   // a nested case and a ternary in a branch
    s = 4'd9;
    case (st)
      2'd0: if (a) s = 4'd1; else s = 4'd0;
      2'd1: s = a ? 4'd4 : 4'd5;
      2'd2: ;
      2'd3: s = 4'd9;
    endcase
  end
  always_comb begin   // a break out of a while loop is not merged
    m = 4'd0;
    while (m < 4'd8) begin
      if (req[m]) break;
      m = m + 4'd1;
    end
  end
  always @(posedge clk) if (b) later(4'h5);   // a task with <= is not merged
  always @(negedge clk) begin   // negedge, and two blocks of one process
    if (a) f1 <= 4'hc; else f1 <= 4'h3;
    f2 <= f1;
    if (b) f3 <= #1 4'h6;
  end
  initial begin
    a = 0; b = 0; sel = 0; req = 8'h10; st = 0; clk = 1; #1;
    $display("y=%b z=%b w=%b idx=%0d u=%0d k=%0d s=%0d", y, z, w, idx, u, k, s);
    a = 1'bx; #1 $display("a=x: y=%b z=%b u=%b s=%b", y, z, u, s);
    b = 1'bx; #1 $display("a=x b=x: y=%b k=%b", y, k);
    a = 0; #1 $display("b=x: y=%b k=%b", y, k);
    st = 2'bx1; #1 $display("st=x1: k=%b s=%b", k, s);
    b = 1; st = 2'b1x; #1 $display("st=1x: k=%b s=%b", k, s);
    sel = 3'bx10; #1 $display("sel=x10: w=%b", w);
    sel = 3'b0x1; #1 $display("sel=0x1: w=%b", w);
    sel = 3'b1zx; #1 $display("sel=1zx: w=%b", w);
    req = 8'bx000_0000; #1 $display("req=x0000000: idx=%0d", idx);
    req = 8'b0000_1x00; #1 $display("req=00001x00: idx=%0d", idx);
    a = 1; clk = 0; #1 $display("f1=%h f2=%h", f1, f2);
    clk = 1; #1 a = 1'bx; clk = 0; #1 $display("a=x: f1=%b f2=%h", f1, f2);
    clk = 1; #1 a = 0; clk = 1'bx; #1 $display("clk 1->x: f1=%b f2=%b", f1, f2);
    clk = 0; #1 $display("clk x->0: f1=%b f2=%b", f1, f2);
    clk = 1; #1 clk = 0; #1 $display("f1=%b f2=%b", f1, f2);
    clk = 1; b = 1; #1 clk = 0; #2 $display("f3=%b", f3);
    clk = 1; b = 1'bx; #1 clk = 0; #2 $display("b=x: f3=%b", f3);
    $finish;
  end
endmodule
//> y=0111 z=0110 w=0000 idx=4 u=2 k=0 s=0
//> a=x: y=0xx1 z=0xxx u=001x s=000x
//> a=x b=x: y=0xx1 k=000x
//> b=x: y=0x11 k=000x
//> st=x1: k=00xx s=xx01
//> st=1x: k=0011 s=1001
//> sel=x10: w=xx00
//> sel=0x1: w=0x00
//> sel=1zx: w=1000
//> req=x0000000: idx=X
//> req=00001x00: idx=X
//> Warning: "checks-fallback.sv", 12: unique if: more than one condition is true at time 11
//> f1=c f2=x
//> a=x: f1=xxxx f2=c
//> clk 1->x: f1=xxxx f2=xxxx
//> clk x->0: f1=xxxx f2=xxxx
//> f1=0011 f2=xxxx
//> f3=0110
//> b=x: f3=xxxx
