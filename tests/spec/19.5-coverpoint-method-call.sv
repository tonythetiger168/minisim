// IEEE 1800-2023 19.5: a coverpoint expression, and its iff, may call a
// function or a class method; the call runs at each sample.
//! icarus-skip: Icarus 13 does not support covergroups
class item;
  bit bits[$];
  function int ones();
    int n = 0;
    foreach (bits[i]) n += bits[i];
    return n;
  endfunction
  function bit valid();
    return bits.size() != 0;
  endfunction
endclass
function automatic int twice(int x);
  return 2 * x;
endfunction
class monitor;
  item tr;
  int calls = 0;
  covergroup cg;
    cp_ones : coverpoint tr.ones() iff (tr.valid()) { bins none = {0}; bins some = {[1:3]}; bins many = {[4:$]}; }
    cp_any : coverpoint (tr.ones() > 0) { bins yes = {1}; bins no = {0}; }
    cp_fn : coverpoint twice(tr.bits.size()) { bins b[] = {0, 2, 4, 6}; }
  endgroup
  function new();
    cg = new;
    tr = new;
  endfunction
endclass
module top;
  initial begin
    monitor m = new;
    m.cg.sample();                    // empty: cp_ones skipped by iff
    m.tr.bits = '{1, 0, 1};
    m.cg.sample();
    m.tr.bits = '{0};
    m.cg.sample();
    $display("ones %0.1f any %0.1f fn %0.1f", m.cg.cp_ones.get_coverage(), m.cg.cp_any.get_coverage(),
             m.cg.cp_fn.get_coverage());
  end
endmodule
//> ones 66.7 any 100.0 fn 75.0
