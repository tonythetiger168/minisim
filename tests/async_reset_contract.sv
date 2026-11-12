`timescale 1ns/1ps
module tb;
reg clk=0, rst_n=1;
reg [3:0] q;
integer checks=0;
always @(posedge clk or negedge rst_n)
  if (!rst_n) q <= 0; else q <= q+1;
task check(input [3:0] expected);
  begin
    #1;
    if (q !== expected) $fatal(1,"q=%h expected=%h",q,expected);
    checks=checks+1;
    $display("CHECK %0d t=%0t q=%0d",checks,$time,q);
  end
endtask
initial begin
  #2 rst_n=0; check(0); // reset without clock
  rst_n=1; check(0); // release alone does not increment
  clk=1; check(1);
  clk=0; #1; clk=1; rst_n=0; check(0); // simultaneous events: reset wins
  clk=0; rst_n=1; #1; clk=1; check(1);
  clk=0; #1; rst_n=0; check(0); // re-arm after prior simultaneous wake
  if(checks!=6) $fatal(1,"empty/incomplete checks");
  $display("PASS 6 async-reset contract checks");
  $finish;
end
endmodule
