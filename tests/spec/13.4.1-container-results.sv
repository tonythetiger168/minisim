// IEEE 1800-2023 13.4.1, 7.10, 6.21: functions that return a queue or an
// associative array (typedef'd return types, also virtual methods), and a
// static container inside a function, which keeps its contents across calls.
//! icarus-skip: Icarus 13 does not support functions returning queues
module top;
  typedef int iq_t[$];
  typedef string ss_t[string];
  function automatic iq_t upto(int n);
    for (int i = 0; i < n; i++) upto.push_back(i * i);
  endfunction
  function automatic ss_t names();
    ss_t r;
    r["a"] = "alpha"; r["b"] = "beta";
    return r;
  endfunction
  function int remember(int v);
    static int seen[$];
    seen.push_back(v);
    return seen.size();
  endfunction
  class B; virtual function iq_t get(); return '{1}; endfunction endclass
  class D extends B; virtual function iq_t get(); return '{4, 5, 6}; endfunction endclass
  iq_t q;
  ss_t s;
  B h;
  D dd;
  initial begin
    q = upto(4);
    $display("%0d %0d %0d", q.size(), q[3], upto(3).size());
    s = names();
    $display("%0d %s", s.num(), s["b"]);
    void'(remember(1)); void'(remember(2));
    $display("%0d", remember(3));
    dd = new; h = dd;
    q = h.get();
    $display("%0d %0d", q.size(), q[2]);
  end
endmodule

//> 4 9 3
//> 2 beta
//> 3
//> 3 6
