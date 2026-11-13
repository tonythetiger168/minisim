// msim -xprop: a branch that calls a task or a function with side effects
// is merged like any other: the variables the call writes take the merge.
// A task with a nonblocking assignment is merged too: its target becomes x.
// A task with a delay is still not merged.
//! args: -xprop
module top;
  logic c, d;
  logic [7:0] a, b, cnt, n, t, q;
  task automatic set_ab(input logic [7:0] v);
    a = v;
    b = v + 8'd1;
  endtask
  function automatic logic [7:0] bump();
    cnt = cnt + 8'd1;   // a static variable written by a function
    return cnt;
  endfunction
  task automatic later(input logic [7:0] v);
    q <= v;
  endtask
  always_comb begin
    a = 8'h00; b = 8'h00;
    if (c) set_ab(8'h10); else set_ab(8'h12);
  end
  always @(d) begin
    if (c) n = bump();
    else n = 8'h00;
  end
  always @(d) begin
    if (c) later(8'h55);
    t = 8'h01;
  end
  initial begin
    cnt = 8'h04; q = 8'h00;
    c = 1; d = 0; #1 d = 1; #1;
    $display("a=%h b=%h n=%h cnt=%h q=%h", a, b, n, cnt, q);
    c = 1'bx; #1 d = 0; #1;
    $display("a=%b b=%b n=%b cnt=%b q=%h", a, b, n, cnt, q);
    $finish;
  end
endmodule
//> a=10 b=11 n=06 cnt=06 q=55
//> a=000100x0 b=000100x1 n=00000xxx cnt=0000011x q=xx
