// IEEE 1800-2023 19.4 / 19.5: a coverpoint of a covergroup in a class may
// read an element of a dynamic array through a chain of handles
// (tr.txn.data[0]), with an iff that guards the empty array.
//! icarus-skip: Icarus 13 does not support covergroups
class txn_t; bit write; logic [63:0] data[]; endclass
class item; txn_t txn = new; endclass
class cov;
  item tr;
  covergroup cg_data;
    cp_data: coverpoint tr.txn.data[0] iff (tr.txn.data.size() > 0) { bins zero = {64'h0}; bins other = default; }
    cp_w: coverpoint tr.txn.write;
    cx: cross cp_data, cp_w;
  endgroup
  function new(); cg_data = new(); endfunction
  function void write(item t); tr = t; cg_data.sample(); endfunction
endclass
module top;
  initial begin
    static cov c = new; static item i = new;
    c.write(i);
    i.txn.data = new[1]; i.txn.data[0] = 0; c.write(i);
    $display("%0.1f %0.1f", c.cg_data.cp_data.get_inst_coverage(), c.cg_data.get_inst_coverage());
    i.txn.data[0] = 5; i.txn.write = 1; c.write(i);
    $display("%0.1f %0.1f", c.cg_data.cp_data.get_inst_coverage(), c.cg_data.get_inst_coverage());
  end
endmodule
//> 100.0 66.7
//> 100.0 83.3
