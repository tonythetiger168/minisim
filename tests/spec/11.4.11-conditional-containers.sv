// IEEE 1800-2023 11.4.11: the conditional operator on queues and dynamic
// arrays. A known condition gives one arm's value; an x condition combines
// arms of the same size element by element, an element that differs reading
// as its type's default (Table 7-1), and arms of different sizes give an
// empty container.
//! icarus-skip: Icarus does not support ?: on queue operands
module t;
  function automatic string show(int q[$]);
    string s = "";
    foreach (q[i]) s = {s, $sformatf(" %0d", q[i])};
    return {$sformatf("%0d:", q.size()), s};
  endfunction
  task automatic take(int q[$]);
    $display("take %s", show(q));
  endtask
  int a[$] = '{1, 2, 3};
  int b[$] = '{7, 2, 9};
  int s[$] = '{5};
  int r[$];
  int d1[], d2[], d3[];
  logic c;
  logic [3:0] la[$] = '{4'h1, 4'h2};
  logic [3:0] lb[$] = '{4'h1, 4'h3};
  logic [3:0] lr[$];
  initial begin
    c = 1; r = c ? a : b; $display("true %s", show(r));
    c = 0; r = c ? a : b; $display("false %s", show(r));
    c = 1; take(c ? s : a);
    c = 0; take(c ? s : a);
    c = 1'bx; r = c ? a : b; $display("x same size %s", show(r));
    c = 1'bx; r = c ? a : s; $display("x other size %s", show(r));
    c = 1'bz; lr = c ? la : lb; $display("x logic %0d: %h %h", lr.size(), lr[0], lr[1]);
    d1 = '{4, 5}; d2 = '{6};
    c = 0; d3 = c ? d1 : d2; $display("dyn %0d %0d", d3.size(), d3[0]);
  end
endmodule
//> true 3: 1 2 3
//> false 3: 7 2 9
//> take 1: 5
//> take 3: 1 2 3
//> x same size 3: 0 2 0
//> x other size 0:
//> x logic 2: 1 x
//> dyn 1 6
