// 7.4 / 6.20.2: a parameter unpacked array (a ROM table) read with an index
// known only at run time.
module top;
  localparam logic [7:0] ROM [0:5] = '{8'h10, 8'h21, 8'h32, 8'h43, 8'h54, 8'h65};
  localparam int DESC [3:0] = '{40, 30, 20, 10};
  int i;
  initial begin
    for (i = 0; i < 6; i++) $display("rom[%0d]=%h", i, ROM[i]);
    for (i = 0; i < 4; i++) $display("desc[%0d]=%0d", i, DESC[i]);
  end
endmodule
//> rom[0]=10
//> rom[1]=21
//> rom[2]=32
//> rom[3]=43
//> rom[4]=54
//> rom[5]=65
//> desc[0]=10
//> desc[1]=20
//> desc[2]=30
//> desc[3]=40
