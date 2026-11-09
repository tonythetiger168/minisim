// IEEE 1800-2023 13.4.2, 13.3.2: An automatic task can call itself, with a
// delay between the calls; each level keeps its own arguments.
module top;
  task automatic down(input int n);
    if (n == 0) return;
    #1;
    down(n - 1);
    $display("%0t: back at %0d", $time, n);
  endtask
  initial down(3);
endmodule

//> 3: back at 1
//> 3: back at 2
//> 3: back at 3
