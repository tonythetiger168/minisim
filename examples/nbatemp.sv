// A block woken by any change that keeps a working value in a module
// variable and schedules nonblocking assignments (the shape msim gives VHDL
// processes): msim.cpp localizeSeqBlocking makes the working value silent
// and the block joins the static clocked schedule. Every mode prints the same.
module nbatemp;
  logic [7:0] a = 0, b = 0, y, z;
  logic [7:0] t;   // written and read only by the block below
  int n = 0;
  always @(a or b) begin
    t = a ^ b;
    y <= t;
    z <= ~t + a;
  end
  always @(y or z) n++;
  initial begin
    repeat (6) begin
      #1 a = a * 3 + 1;
      #1 b = b + a;
    end
    #1 $display("y=%h z=%h n=%0d", y, z, n);
  end
endmodule
