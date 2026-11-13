-- Generic packages (VHDL-2008 clause 4.7, 6.5.6.2): a generic package with a
-- type, a constant and a function generic, two instances at library level,
-- a protected type inside the generic package, and the 2008 implicit
-- MINIMUM / MAXIMUM of an array.
package fifo_generic_pkg is
  generic (
    type elem_t;
    constant DEPTH : positive;
    constant INIT : elem_t;
    function to_str(x : elem_t) return string
  );
  type store_t is array (0 to DEPTH - 1) of elem_t;
  type fifo_t is protected
    procedure put(x : elem_t);
    impure function get return elem_t;
    impure function count return natural;
    impure function dump return string;
  end protected;
end package;

package body fifo_generic_pkg is
  type fifo_t is protected body
    variable store : store_t := (others => INIT);
    variable rd, wr, n : natural := 0;
    procedure put(x : elem_t) is
    begin
      store(wr) := x;
      wr := (wr + 1) mod DEPTH;
      n := n + 1;
    end procedure;
    impure function get return elem_t is
      variable r : elem_t;
    begin
      r := store(rd);
      rd := (rd + 1) mod DEPTH;
      n := n - 1;
      return r;
    end function;
    impure function count return natural is
    begin
      return n;
    end function;
    impure function dump return string is
    begin
      return to_str(store(rd)) & ".." & to_str(store((rd + n - 1) mod DEPTH));
    end function;
  end protected body;
end package body;

package str_pkg is
  function int_str(x : integer) return string;
  function bool_str(x : boolean) return string;
end package;

package body str_pkg is
  function int_str(x : integer) return string is
  begin
    return integer'image(x);
  end function;
  function bool_str(x : boolean) return string is
  begin
    if x then
      return "T";
    end if;
    return "F";
  end function;
end package body;

use work.str_pkg.all;
package int_fifo_pkg is new work.fifo_generic_pkg
  generic map (elem_t => integer, DEPTH => 4, INIT => -1, to_str => int_str);

use work.str_pkg.all;
package bool_fifo_pkg is new work.fifo_generic_pkg
  generic map (elem_t => boolean, DEPTH => 2, INIT => false, to_str => bool_str);

entity generic_pkg_tb is
end entity;

architecture sim of generic_pkg_tb is
begin
  process
    variable fi : work.int_fifo_pkg.fifo_t;
    variable fb : work.bool_fifo_pkg.fifo_t;
    variable v : integer;
    constant iv : integer_vector := (7, -3, 12, 5);
  begin
    for i in 1 to 3 loop
      fi.put(i * 100);
    end loop;
    v := fi.get;
    report "int got=" & integer'image(v) & " count=" & integer'image(fi.count) & " dump=" & fi.dump;
    fb.put(true);
    fb.put(false);
    report "bool count=" & integer'image(fb.count) & " first=" & boolean'image(fb.get) & " dump=" & fb.dump;
    report "min=" & integer'image(minimum(iv)) & " max=" & integer'image(maximum(iv));
    wait;
  end process;
end architecture;
