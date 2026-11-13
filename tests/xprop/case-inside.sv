// msim -xprop: case ... inside (12.5.4) with an unknown selector. An item
// may match when ==? or its range compare is x; every item that may match
// before the first that does runs, and the results merge.
//! args: -xprop
module top;
  logic [3:0] s; logic [7:0] y, z;
  always_comb begin
    unique case (s) inside
      4'b00??: y = 8'h11;
      [4'd4:4'd7]: y = 8'h13;
      4'd8, 4'd9: y = 8'h17;
      default: y = 8'hff;
    endcase
  end
  always_comb begin
    z = 8'h00;
    case (s) inside
      4'b1?1?: z = 8'h0f;
      4'b0001: z = 8'h01;
    endcase
  end
  initial begin
    s = 4'd2; #1 $display("%h %h", y, z);
    s = 4'd5; #1 $display("%h %h", y, z);
    s = 4'b00x1; #1 $display("%b %b", y, z);
    s = 4'b100x; #1 $display("%b %b", y, z);
    s = 4'b1x1x; #1 $display("%b %b", y, z);
    s = 4'bxxxx; #1 $display("%b %b", y, z);
    $finish;
  end
endmodule
//> 11 00
//> 13 00
//> 00010001 0000000x
//> xxx1xx11 00000000
//> xxx1xx11 00001111
//> xxx1xxx1 0000xxxx
