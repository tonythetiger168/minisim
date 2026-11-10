// IEEE 1800-2023 10.3.2, 13.4: several continuous assignments can call the
// same function. Each one is evaluated when one of its operands changes; a
// call's argument and result variables are not operands, so two assigns that
// share a function (static or automatic) do not wake each other forever.
module top;
  function automatic logic [7:0] rom(input logic [1:0] i);
    case (i) 0: rom = 8'h11; 1: rom = 8'h22; default: rom = 8'h33; endcase
  endfunction
  function logic [7:0] srom(input logic [1:0] i);
    case (i) 0: srom = 8'h44; 1: srom = 8'h55; default: srom = 8'h66; endcase
  endfunction
  logic [7:0] k = 1;
  function automatic logic [7:0] addk(input logic [7:0] v);
    return v + k;
  endfunction
  logic [1:0] a = 0, b = 1;
  logic [7:0] x, y, sx, sy, z, w;
  assign x = rom(a);
  assign y = rom(b);
  assign sx = srom(a);
  assign sy = srom(b);
  assign z = addk(x);
  assign w = addk(y) + addk(8'd1);
  initial begin
    #1 $display("%h %h %h %h %h %h", x, y, sx, sy, z, w);
    a = 2;
    #1 $display("%h %h %h %h %h %h", x, y, sx, sy, z, w);
    b = 0;
    #1 $display("%h %h %h %h %h %h", x, y, sx, sy, z, w);
  end
endmodule
//> 11 22 44 55 12 25
//> 33 22 66 55 34 25
//> 33 11 66 44 34 14
