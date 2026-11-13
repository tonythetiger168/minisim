// IEEE 1800-2023 19.5 Coverpoints, 19.4 covergroups in classes: a coverpoint
// expression and its iff guard may read a queue reached through handles of
// the object; the guard keeps the expression from being read when it fails.
//! icarus-skip: Icarus 13 has no covergroups
class txn_c;
  bit write;
  bit [7:0] data[$];
endclass
class item;
  txn_c txn;
endclass
class cov;
  item tr;
  covergroup cg;
    cp_data: coverpoint tr.txn.data[0] iff (tr.txn != null && tr.txn.data.size() > 0) {
      bins zero = {0}; bins ones = {8'hFF}; bins other = default;
    }
    cp_n: coverpoint tr.txn.data.size() iff (tr.txn != null) { bins none = {0}; bins one = {1}; bins more = {[2:$]}; }
  endgroup
  function new();
    cg = new();
  endfunction
  function void write(item t);
    tr = t;
    cg.sample();
  endfunction
endclass
module t;
  initial begin
    cov c = new;
    item it = new;
    c.write(it);                       // txn is null: both guards fail
    $display("%0.1f", c.cg.get_inst_coverage());
    it.txn = new;
    c.write(it);                       // empty queue
    $display("%0.1f", c.cg.get_inst_coverage());
    it.txn.data.push_back(8'hFF);
    c.write(it);
    $display("%0.1f", c.cg.get_inst_coverage());
    it.txn.data.push_front(0);
    c.write(it);
    $display("%0.1f", c.cg.get_inst_coverage());
  end
endmodule
//> 0.0
//> 16.7
//> 58.3
//> 100.0
