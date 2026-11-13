// IEEE 1800-2023 9.4.2.3 Conditional event controls: @(event iff expr) only
// counts the event when expr is true as it happens. Also through a virtual
// interface, as a UVM driver waits for reset to end.
//! icarus-skip: Icarus 13 has no classes or virtual interfaces
interface bus (input logic clk, input logic rst_n);
endinterface
class drv;
  virtual bus vif;
  task run();
    @(posedge vif.clk iff vif.rst_n === 1'b1);
    $display("class woke at %0t", $time);
  endtask
endclass
module t;
  logic clk = 0, rst_n = 0, en = 0;
  int n;
  always #5 clk = ~clk;
  bus b (clk, rst_n);
  initial begin repeat (5) @(posedge clk); rst_n <= 1; end
  initial begin
    drv d = new;
    d.vif = b;
    fork d.run(); join_none
    @(posedge clk iff rst_n === 1'b1);
    $display("module woke at %0t", $time);
    repeat (3) @(posedge clk iff en) n++;
    $display("n=%0d at %0t", n, $time);
    $finish;
  end
  initial begin #72 en = 1; #20 en = 0; #20 en = 1; end
endmodule
//> module woke at 55
//> class woke at 55
//> n=3 at 115
