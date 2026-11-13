// IEEE 1800-2023 19.3: a covergroup sampled at block events. @@(begin f)
// samples when task or function f starts, @@(end f) when it finishes,
// including through a return; `or` joins several block events.
//! icarus-skip: Icarus 13 has no covergroups
module top;
  int v;
  task automatic send(int x);
    v = x;
    if (x == 3) return;
    v = x + 10;
  endtask
  function int peek(int a); return a + v; endfunction
  covergroup cg_end @@(end send);
    coverpoint v { bins b[] = {[1:4], [11:14]}; }
  endgroup
  covergroup cg_begin @@(begin send or begin peek);
    coverpoint v { bins b[] = {[0:14]}; }
  endgroup
  cg_end ce = new;
  cg_begin cb = new;
  initial begin
    send(1);           // end: 11; begin: 0
    send(2);           // end: 12; begin: 11
    send(3);           // end: 3 (return); begin: 12
    void'(peek(0));    // begin: 3
    $display("%0.2f %0.2f", ce.get_inst_coverage(), cb.get_inst_coverage());
  end
endmodule
//> 37.50 26.67
