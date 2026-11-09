// IEEE 1800-2023 21.2.1.6: %p of containers whose elements are unpacked
// structs (nested, with enum and string members), enums (a value with no
// name prints as a number), and packed structs; an associative array with
// string keys, and a fixed-size array of queues of structs.
//! icarus-skip: Icarus 13 does not print %p of arrays
typedef enum {IDLE, BUSY = 3} st_t;
typedef struct { int a; st_t s; string n; } rec_t;
typedef struct { bit [3:0] x; rec_t r; } outer_t;
typedef struct packed { bit [3:0] hi, lo; } pk_t;
module top;
  rec_t q[$];
  st_t e[];
  outer_t o[string];
  pk_t pq[$];
  rec_t m[2][$];
  initial begin
    q.push_back('{1, BUSY, "x"}); q.push_back('{-2, IDLE, "yy"});
    e = new[3]; e[1] = BUSY; e[2] = st_t'(7);
    o["k"] = '{5, '{3, BUSY, "z"}};
    pq = '{'{1, 2}, '{3, 4}};
    m[1].push_back('{9, IDLE, ""});
    $display("%p", q); $display("%p", e); $display("%p", o); $display("%p", pq); $display("%p", m);
  end
endmodule

//> '{'{a:1, s:BUSY, n:"x"}, '{a:-2, s:IDLE, n:"yy"}}
//> '{IDLE, BUSY, 7}
//> '{"k":'{x:5, r:'{a:3, s:BUSY, n:"z"}}}
//> '{'{hi:1, lo:2}, '{hi:3, lo:4}}
//> '{'{}, '{'{a:9, s:IDLE, n:""}}}
