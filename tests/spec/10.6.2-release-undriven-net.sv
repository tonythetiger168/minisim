// 10.6.2: releasing a forced net gives it back to its drivers. A net with no
// drivers at all resolves to z again (6.6.1), and a tri1 net to its pull-up.
module top;
  wire w;
  tri1 t;
  initial begin
    force w = 1'b1;
    force t = 1'b0;
    #1 $display("forced w=%b t=%b", w, t);
    release w;
    release t;
    #1 $display("released w=%b t=%b", w, t);
  end
endmodule
//> forced w=1 t=0
//> released w=z t=1
