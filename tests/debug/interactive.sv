// msim -i self-test: run control, stops, force, and class objects.
package p;
  class item;
    int id; logic [7:0] data; string tag;
    item next;
    function new(int i); id = i; data = 8'h10 + i; tag = $sformatf("it%0d", i); endfunction
  endclass
  class big extends item;
    bit flag = 1;
    function new(int i); super.new(i); endfunction
  endclass
endpackage
module tb;
  import p::*;
  logic clk = 0, en = 0;
  logic [7:0] cnt = 0;
  item head, none;
  big b;
  always #5 clk = ~clk;
  always @(posedge clk) if (en) cnt <= cnt + 1;
  initial begin
    head = new(1); b = new(2); head.next = b;
    #12 en = 1;
    #100 $display("cnt=%0d", cnt);
    $finish;
  end
endmodule
