// Compiled unpacked arrays: element reads with run-time indexes (also out of
// range and X), element and bit writes (blocking and nonblocking, also in a
// clocked process), indexed part-selects, and partial continuous assigns to
// one net. msim -compile must print what the interpreter prints.
module memories;
  reg clk = 0;
  always #5 clk = ~clk;
  reg [7:0] mem [0:15];
  reg [7:0] m2 [3:0][0:1];
  reg [3:0] wa, ra;
  reg [7:0] wd;
  reg [15:0] v;
  wire [15:0] parts;
  assign parts[7:0] = mem[ra];
  assign parts[15:8] = v[ra +: 8];
  always @(posedge clk) begin
    mem[wa] <= wd;
    v[wa] <= wd[0];
  end
  integer i;
  initial begin
    for (i = 0; i < 16; i = i + 1) mem[i] = i * 3;
    for (i = 0; i < 4; i = i + 1) begin m2[i][0] = i; m2[i][1] = i + 8'h10; end
    v = 16'h0f0f;
    wa = 2; wd = 8'h55; ra = 2;
    @(posedge clk); #1 $display("%h %h %h %h", mem[2], parts, v, m2[3][1]);
    wa = 9; wd = 8'haa; ra = 9;
    @(posedge clk); #1 $display("%h %h %h", mem[9], parts, v[15 -: 4]);
    ra = 4'bx0x1; #1 $display("%h %h", mem[ra], parts);
    mem[1] = 8'h34; ra = 1; #1 $display("%h %h %h", mem[0], mem[1], parts);
    $finish;
  end
endmodule
