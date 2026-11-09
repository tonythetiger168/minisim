// IEEE 1800-2023 8.26: Interface classes declare pure virtual methods and
// parameters; a class implements one or more of them and can be used through
// a handle of each interface class type. Interface classes can extend
// other interface classes.
module top;
  interface class PutImp #(type PUT_T = logic);
    pure virtual function void put(PUT_T a);
  endclass
  interface class GetImp #(type GET_T = logic);
    pure virtual function GET_T get();
  endclass
  interface class Sized;
    localparam int LIMIT = 4;
    pure virtual function int size();
  endclass
  interface class Fifo #(type T = int) extends PutImp#(T), GetImp#(T), Sized;
  endclass
  class MyQueue #(type T = int, int DEPTH = 1) implements Fifo#(T);
    T m_q[$];
    virtual function void put(T a); if (m_q.size() < DEPTH) m_q.push_back(a); endfunction
    virtual function T get(); return m_q.pop_front(); endfunction
    virtual function int size(); return m_q.size(); endfunction
  endclass
  class Logger implements Sized;
    virtual function int size(); return 99; endfunction
  endclass
  MyQueue #(int, 3) q;
  PutImp #(int) p;
  GetImp #(int) g;
  Sized s[2];
  Logger lg;
  initial begin
    q = new;
    p = q;
    g = q;
    p.put(1); p.put(2); p.put(3); p.put(4);
    lg = new;
    s[0] = q; s[1] = lg;
    $display("%0d %0d %0d %0d", g.get(), s[0].size(), s[1].size(), Sized::LIMIT);
    if ($cast(q, s[0])) $display("cast ok %0d", q.get());
    if (!$cast(q, s[1])) $display("cast failed");
  end
endmodule

//> 1 2 99 4
//> cast ok 2
//> cast failed
