// IEEE 1800-2023 15.4: a parameterized mailbox whose message type is an
// array: fixed-size (one and two dimensions), dynamic array, or queue.
// put copies the whole array in, get / peek copy it out, and the sender
// may change its own array afterwards without changing the message.
//! icarus-skip: Icarus 13 has no parameterized mailbox
module top;
  typedef int vec4_t[4];
  typedef bit [7:0] grid_t[2][3];
  typedef shortint dyn_t[];
  typedef string names_t[$];
  mailbox #(vec4_t) mv = new(1);
  mailbox #(grid_t) mg = new;
  mailbox #(dyn_t) md = new;
  mailbox #(names_t) mn = new;
  initial begin : producer
    vec4_t v = '{10, 20, 30, 40};
    grid_t g = '{'{1, 2, 3}, '{4, 5, 6}};
    dyn_t d = '{-1, -2, -3};
    names_t n = '{"ab", "cd"};
    mv.put(v);
    v[0] = 11;
    mv.put(v);   // the mailbox holds one: waits for the consumer
    $display("%0t: second put done", $time);
    mg.put(g);
    md.put(d); d = '{7};
    mn.put(n); n.push_back("ef");
    mn.put(n);
  end
  initial begin : consumer
    vec4_t r;
    grid_t gr;
    dyn_t dr;
    names_t nr;
    int ok;
    #5 mv.peek(r);
    $display("%0t: peek %p num=%0d", $time, r, mv.num());
    mv.get(r);
    $display("%0t: get %p", $time, r);
    #1 mv.get(r);
    $display("%0t: get %p", $time, r);
    ok = mv.try_get(r);
    $display("try_get on empty: %0d", ok);
    mg.get(gr);
    $display("grid %p", gr);
    md.get(dr);
    $display("dyn %p size=%0d", dr, dr.size());
    ok = mn.try_peek(nr);
    $display("try_peek %0d %p", ok, nr);
    mn.get(nr);
    mn.get(nr);
    $display("names %p", nr);
  end
endmodule
//> 5: peek '{10, 20, 30, 40} num=1
//> 5: get '{10, 20, 30, 40}
//> 5: second put done
//> 6: get '{11, 20, 30, 40}
//> try_get on empty: 0
//> grid '{'{1, 2, 3}, '{4, 5, 6}}
//> dyn '{-1, -2, -3} size=3
//> try_peek 1 '{"ab", "cd"}
//> names '{"ab", "cd", "ef"}
