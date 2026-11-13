// IEEE 1800-2023 7.2: an unpacked structure can hold dynamic arrays, queues
// and associative arrays as members. Struct assignment, argument passing
// and return copy the members, so each copy owns its own containers.
//! icarus-skip: Icarus does not support unpacked structs
module t;
  typedef struct { int id; byte data[$]; } pkt_t;
  typedef struct { pkt_t p; int tags[string]; shortint d[]; } frame_t;
  class Holder; pkt_t pk; endclass
  pkt_t qq[$];
  function automatic pkt_t mk(int id, int n);
    pkt_t r;
    r.id = id;
    for (int i = 0; i < n; i++) r.data.push_back(8'(i + id));
    return r;
  endfunction
  function automatic int total(pkt_t x);
    int s = 0;
    foreach (x.data[i]) s += x.data[i];
    x.data.delete();
    return s;
  endfunction
  task automatic fill(output pkt_t o, input int n);
    for (int i = 0; i < n; i++) o.data.push_back(8'(i * 2));
    o.id = n;
  endtask
  initial begin
    pkt_t a, b, c;
    frame_t f, g;
    Holder h = new;
    a = mk(10, 3);
    b = a;
    b.data[0] = 99;
    $display("a=%p b=%p", a, b);
    $display("total=%0d after=%0d", total(a), a.data.size());
    qq.push_back(a); qq.push_back(b);
    a.data.push_back(1);
    $display("qq[0].data=%p qq[1].data[0]=%0d n=%0d", qq[0].data, qq[1].data[0], qq.size());
    f.p = a;
    f.tags["x"] = 1; f.tags["y"] = 2;
    f.d = new[2]; f.d[1] = -3;
    g = f;
    g.tags["z"] = 3; g.p.data.delete(0);
    $display("f: %0d %0d %0d %0d | g: %0d %0d", f.tags.num(), f.p.data.size(), f.d[1], f.d.size(), g.tags.num(), g.p.data.size());
    b = '{id: 7, data: '{8'h1, 8'h2}};
    $display("pattern %p", b);
    h.pk = b;
    h.pk.data.push_back(3);
    $display("class %0d %0d", h.pk.data.size(), b.data.size());
    fill(c, 3);
    $display("out %0d %p", c.id, c.data);
  end
endmodule
//> a='{id:10, data:'{10, 11, 12}} b='{id:10, data:'{99, 11, 12}}
//> total=33 after=3
//> qq[0].data='{10, 11, 12} qq[1].data[0]=99 n=2
//> f: 2 4 -3 2 | g: 3 3
//> pattern '{id:7, data:'{1, 2}}
//> class 3 2
//> out 3 '{0, 2, 4}
