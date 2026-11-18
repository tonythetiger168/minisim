//! icarus-skip: Icarus has no typedef of a virtual interface
// IEEE 1800-2023 25.9: an array of virtual interfaces takes an array of
// interface instances (whole, a slice, a row of a 2-D array, or through a
// hierarchical name), element by element from the left, as an unpacked
// array assignment (7.6). Also as a task argument and a class property.
interface bus_if;
  int id;
endinterface
typedef virtual bus_if vbus;
module leaf;
  bus_if inner[2] ();
  initial begin inner[0].id = 90; inner[1].id = 91; end
endmodule
class Holder;
  vbus v[3];
  function new(vbus a[3]); v = a; endfunction
  function int sum(); return v[0].id + v[1].id + v[2].id; endfunction
endclass
module t;
  bus_if up[0:2] ();
  bus_if dn[2:0] ();
  bus_if grid[1:0][0:1] ();
  leaf u_leaf ();
  vbus a[3], b[2], g[2][2];
  Holder h;
  for (genvar i = 0; i < 3; i++) begin : init
    initial begin up[i].id = 10 + i; dn[i].id = 20 + i; end
  end
  for (genvar i = 0; i < 2; i++) begin : gi
    for (genvar j = 0; j < 2; j++) begin : gj
      initial grid[i][j].id = 30 + 2 * i + j;
    end
  end
  initial begin
    #1;
    a = up;
    $display("%0d %0d %0d", a[0].id, a[1].id, a[2].id);
    a = dn;
    $display("%0d %0d %0d", a[0].id, a[1].id, a[2].id);
    b = up[1:2];
    $display("%0d %0d", b[0].id, b[1].id);
    b = grid[0];
    $display("%0d %0d", b[0].id, b[1].id);
    g = grid;
    $display("%0d %0d %0d %0d", g[0][0].id, g[0][1].id, g[1][0].id, g[1][1].id);
    b = u_leaf.inner;
    $display("%0d %0d", b[0].id, b[1].id);
    h = new(up);
    $display("%0d", h.sum());
  end
endmodule
//> 10 11 12
//> 22 21 20
//> 11 12
//> 30 31
//> 32 33 30 31
//> 90 91
//> 33
