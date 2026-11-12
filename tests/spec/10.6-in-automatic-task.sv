// IEEE 1800-2023 10.6: assign / deassign and force / release in an
// automatic task. The statements act on static variables and nets; the
// right-hand side keeps being evaluated after the task waits.
module top;
  reg r; wire w; assign w = 0;
  reg a = 1;
  task automatic t(input int n);
    force r = a;
    force w = a;
    #1 $display("%b %b", r, w);
    a = 0;
    #1 $display("%b %b", r, w);
    release r; release w;
    assign r = ~a;
    #1 $display("%b %b", r, w);
    deassign r;
    a = 1;
    #1 $display("%b", r);
  endtask
  initial t(1);
endmodule
//> 1 1
//> 0 0
//> 1 0
//> 1
