// IEEE 1800-2023 11.4.7: && and || do not evaluate the right operand when the
// left one decides the result, so a null handle on the left guards an access
// through it on the right.
//! icarus-skip: Icarus 13 does not support associative array properties through handles
module top;
  class Node;
    Node parent;
    bit children[Node];
  endclass
  Node n, m;
  int calls;
  function bit touch(); calls++; return 1; endfunction
  initial begin
    n = new;
    $display("%0d", (n.parent != null) && n.parent.children.exists(n));
    m = new; n.parent = m; m.children[n] = 1;
    $display("%0d", (n.parent != null) && n.parent.children.exists(n));
    $display("%0d %0d", 1'b1 || touch(), 1'b0 && touch());
    $display("%0d", calls);
  end
endmodule

//> 0
//> 1
//> 1 0
//> 0
