// IEEE 1800-2023 10.6.1: Procedural continuous assign and deassign on a
// variable (10.6.1).
module top;
  logic q = 0, d = 1;
  initial begin
    assign q = d;
    #1 $display("%b", q);
    d = 0; #1 $display("%b", q);
    q = 1; #1 $display("%b", q);
    deassign q;
    q = 1; #1 $display("%b", q);
  end
endmodule

//> 1
//> 0
//> 0
//> 1
