-- A package generic whose generic types and subprograms the user reaches
-- through 'use n.all' (6.5.6.4). GHDL 4.1 cannot analyze this, so the
-- expected output was worked out from the LRM.
package num_g is
  generic (type t; function add(a, b : t) return t; function img(x : t) return string);
  function twice(x : t) return t;
end;
package body num_g is
  function twice(x : t) return t is begin return add(x, x); end;
end;
package acc_g is
  generic (package n is new work.num_g generic map (<>));
  use n.all;
  function sum3(a, b, c : t) return t;
  function show(x : t) return string;
end;
package body acc_g is
  function sum3(a, b, c : t) return t is begin return add(add(a, b), c); end;
  function show(x : t) return string is begin return "<" & n.img(twice(x)) & ">"; end;
end;
package ops is
  function iadd(a, b : integer) return integer;
  function iimg(x : integer) return string;
end;
package body ops is
  function iadd(a, b : integer) return integer is begin return a + b; end;
  function iimg(x : integer) return string is begin return integer'image(x); end;
end;
use work.ops.all;
package int_num is new work.num_g generic map (t => integer, add => iadd, img => iimg);
package int_acc is new work.acc_g generic map (n => work.int_num);
use work.int_acc.all;
entity pkggen_types_tb is end;
architecture a of pkggen_types_tb is begin
  process begin report integer'image(sum3(1, 2, 3)) & " " & show(21); wait; end process;
end;
