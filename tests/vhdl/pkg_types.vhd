-- packages, functions, procedures, records, arrays, enums, case, loops
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package util_pkg is
  constant DATA_W : natural := 8;
  subtype byte_t is std_logic_vector(DATA_W-1 downto 0);
  type op_t is (op_add, op_sub, op_and, op_or, op_xor);
  type pkt_t is record
    op   : op_t;
    a, b : byte_t;
    tag  : integer range 0 to 15;
  end record;
  type mem_t is array (0 to 15) of byte_t;
  function parity(v : std_logic_vector) return std_logic;
  function alu(p : pkt_t) return byte_t;
  function max3(a, b, c : integer) return integer;
  function reverse(v : std_logic_vector) return std_logic_vector;
  constant INIT_MEM : mem_t := (0 => x"01", 1 => x"02", 5 => x"55", others => x"00");
end package;

package body util_pkg is
  function parity(v : std_logic_vector) return std_logic is
    variable r : std_logic := '0';
  begin
    for i in v'range loop
      r := r xor v(i);
    end loop;
    return r;
  end function;

  function alu(p : pkt_t) return byte_t is
  begin
    case p.op is
      when op_add => return std_logic_vector(unsigned(p.a) + unsigned(p.b));
      when op_sub => return std_logic_vector(unsigned(p.a) - unsigned(p.b));
      when op_and => return p.a and p.b;
      when op_or  => return p.a or p.b;
      when others => return p.a xor p.b;
    end case;
  end function;

  function max3(a, b, c : integer) return integer is
    variable m : integer := a;
  begin
    if b > m then m := b; end if;
    if c > m then m := c; end if;
    return m;
  end function;

  function reverse(v : std_logic_vector) return std_logic_vector is
    variable r : std_logic_vector(v'reverse_range);
  begin
    for i in v'range loop
      r(i) := v(i);
    end loop;
    return r;
  end function;
end package body;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.util_pkg.all;

entity pkg_types is end;

architecture sim of pkg_types is
  signal mem : mem_t := INIT_MEM;
  signal p   : pkt_t;
begin
  process
    variable r   : byte_t;
    variable sum : integer := 0;
    variable v12 : std_logic_vector(11 downto 0) := x"A5C";
  begin
    p <= (op => op_add, a => x"10", b => x"25", tag => 3);
    wait for 1 ns;
    r := alu(p);
    report "add=" & to_hstring(r) & " tag=" & integer'image(p.tag);
    for o in op_t loop
      p.op <= o;
      wait for 1 ns;
      report op_t'image(o) & " -> " & to_hstring(alu(p));
    end loop;
    for i in mem'range loop
      sum := sum + to_integer(unsigned(mem(i)));
    end loop;
    report "sum=" & integer'image(sum) & " max=" & integer'image(max3(3, 9, -2));
    report "parity=" & std_logic'image(parity(x"07")) & " " & std_logic'image(parity(v12));
    report "rev=" & to_string(reverse(x"0F")) & " " & to_hstring(reverse(v12));
    mem(3) <= x"FF";
    wait for 1 ns;
    report "mem3=" & to_hstring(mem(3)) & " high=" & integer'image(mem'high) & " len=" & integer'image(r'length);
    wait;
  end process;
end architecture;
