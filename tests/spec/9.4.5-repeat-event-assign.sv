// IEEE 1800-2023 9.4.5: Intra-assignment repeat event control: the value is
// taken now and written after n events (9.4.5).
module top;
  logic clk = 0;
  always #1 clk = ~clk;
  logic [3:0] q, d = 4'h3;
  initial begin
    q = repeat (2) @(posedge clk) d;
    $display("%0t %h", $time, q);
    q = @(posedge clk) 4'h9;
    $display("%0t %h", $time, q);
    $finish;
  end
endmodule

//> 3 3
//> 5 9
