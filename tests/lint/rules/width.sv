//! args: -only width -enable W002,W006,W007 -top width
module width(input logic [7:0] a, input logic [3:0] n, input logic signed [7:0] s, input logic c,
             output logic [3:0] y, output logic [7:0] z, output logic f);
  logic [3:0] cnt;
  logic [7:0] q;
  logic [15:0] wide;
  always_comb begin
    y = a;                        //~ W001
    y = a[3:0];
    y = cnt + 1;                  // the constant does not widen the sum
    y = 4'd9;
    y = 12;
    y = 300;                      //~ W001
    z = n;                        //~ W002
    z = {4'b0, n};
    z = 8'd3;
    f = (a == n);                 //~ W004
    f = (a == 8'd3);
    f = (n == 5'd20);             //~ W004
    f = (s < a);                  //~ W005
    f = (s < 8'sd3);
    if (a) f = 1'b1;              //~ W006
    if (a != 0) f = 1'b1;
    f = !a;                       //~ W007
    f = a && c;                   //~ W007
    wide = c ? a : n;             //~ W008
    wide = c ? a : 8'd0;
  end
  width_leaf u1(.d(n), .q(q));    //~ W003
  width_leaf u2(.d(a), .q(cnt));  //~ W003
  width_leaf u3(.d(8'd1), .q());
  width_leaf u4(.d(1), .q());     // an unsized constant that fits
endmodule

module width_leaf(input logic [7:0] d, output logic [7:0] q);
  assign q = d;
endmodule
