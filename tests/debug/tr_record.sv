// Transaction recording without UVM (expr.hpp trrec): a stream made with
// $create_transaction_stream, and one named in $begin_transaction with a
// parent transaction. mdebug -tr reads the file msim writes (msim -tr).
module tb;
  logic clk = 0;
  logic [7:0] addr, data;
  integer s, t, p, v;
  always #5 clk = ~clk;
  initial begin
    s = $create_transaction_stream("tb.bus.drive", "TVM");
    repeat (3) begin
      @(posedge clk);
      addr = addr === 8'bx ? 8'h10 : addr + 8'h4;
      data = addr ^ 8'hff;
      t = $begin_transaction(s, "write");
      $add_attribute(t, addr);
      $add_attribute(t, data, "wdata");
      $add_attribute(t, "ok", "status");
      @(posedge clk);
      $end_transaction(t);
      $free_transaction(t);
    end
    p = $begin_transaction("tb.mon.seen", "burst");
    v = $begin_transaction("tb.mon.seen", "beat", 0, p);
    $add_attribute(v, 8'h5a, "byte");
    #10 $end_transaction(v);
    #10 $end_transaction(p);
    #5 $finish;
  end
endmodule
