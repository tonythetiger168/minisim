// IEEE 1800-2023 21.2.1.6: %p of a tagged union prints the tag and the value
// of the member it names ('{M:v}; a void member as '{M}), whether the union
// is packed or unpacked, alone or inside arrays, queues and structs. A null
// chandle prints null.
//! icarus-skip: Icarus 13 does not support tagged unions
//> null
//> '{B:18}
//> '{'{P:'{x:1, y:2}}, '{I:-3}}
//> '{'{None}, '{I:4}}
//> '{t:'{I:9}, n:1}
//> '{C:GREEN}
//> '{R:1.50000}
//> '{S:"hi"}
module top;
  typedef union tagged packed { logic [7:0] A; logic [7:0] B; } pt_t;
  typedef union tagged { void None; int I; struct { int x, y; } P; } tu_t;
  typedef enum {RED, GREEN} col_t;
  typedef union tagged { col_t C; real R; string S; } mix_t;
  chandle ch;
  pt_t p;
  tu_t a[2];
  tu_t q[$];
  struct { tu_t t; int n; } s;
  mix_t m;
  initial begin
    $display("%p", ch);
    p = tagged B 8'h12; $display("%p", p);
    a[0] = tagged P '{1, 2}; a[1] = tagged I -3; $display("%p", a);
    q.push_back(tagged None); q.push_back(tagged I 4); $display("%p", q);
    s.t = tagged I 9; s.n = 1; $display("%p", s);
    m = tagged C GREEN; $display("%p", m);
    m = tagged R 1.5; $display("%p", m);
    m = tagged S "hi"; $display("%p", m);
  end
endmodule
