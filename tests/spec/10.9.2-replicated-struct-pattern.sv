// IEEE 1800-2023 10.9.2, 5.10: '{n{...}} for an unpacked struct gives its
// members the repeated values in order, also nested in arrays of structs.
module t;
  struct {int x, y, z;} xyz = '{3{1}};
  typedef struct {int a, b[4];} ab_t;
  struct {int p; byte q; int r; byte s;} pq = '{2{7, 8'h2}};
  int a = 1, b = 2, c = 3;
  initial begin
    ab_t v[1:0][2:0];
    v = '{2{'{3{'{a, '{2{b, c}}}}}}};
    $display("%0d %0d %0d | %0d %0d %0d %0d", xyz.x, xyz.y, xyz.z, pq.p, pq.q, pq.r, pq.s);
    $display("%0d %0d %0d %0d", v[1][2].a, v[0][0].b[0], v[0][1].b[1], v[1][0].b[3]);
  end
endmodule
//> 1 1 1 | 7 2 7 2
//> 1 2 3 3
//! icarus-skip: Icarus has no unpacked struct patterns
