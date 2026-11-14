-- VHDL-2019: conditional expressions (9.1), empty records (5.3.3), 'image of
-- composite types (16.2.2), interface lists ending in ';' (6.5.6.1) and
-- generic type classes (6.5.3).
library ieee;
use ieee.std_logic_1164.all;

entity vhdl2019c_tb is
end entity;

architecture t of vhdl2019c_tb is
  constant N : integer := 4;
  constant c1 : integer := 10 when N > 3 else 20;
  constant c2 : integer := 1 when N > 9 else 2 when N > 3 else 3;
  type empty_r is record
  end record;
  type pt is record
    x, y : integer;
    c : character;
    s : std_logic_vector(3 downto 0);
  end record;
  type color is (red, green, blue);
  type iv is array (natural range <>) of integer;
  function inc(x : integer;) return integer is
  begin
    return x + 1;
  end function;
  function nxt generic (type E is (<>)) parameter (x : E) return E is
  begin
    if x = E'high then return E'low; else return E'succ(x); end if;
  end function;
  function nc is new nxt generic map (E => color);
  function first generic (type I is (<>); type A is array (I range <>) of type is private) parameter (v : A) return A'element is
  begin
    return v(v'left);
  end function;
  function fi is new first generic map (I => natural, A => iv);
  function maxof generic (type T is range <>) parameter (a, b : T) return T is
  begin
    if a > b then return a; else return b; end if;
  end function;
  function maxi is new maxof generic map (T => integer);
  signal s : std_logic_vector(7 downto 0) := x"A5";
begin
  process
    variable p : pt := (3, -4, 'q', "01XZ");
    variable er : empty_r;
    variable v : integer;
  begin
    report "c1=" & integer'image(c1) & " c2=" & integer'image(c2) & " paren=" & integer'image((5 when c1 = 10 else 6) + 1);
    v := 7 when c2 = 1 else 8;
    report "v=" & integer'image(v) & " inc=" & integer'image(inc(2));
    report "pt=" & pt'image(p);
    report "empty=" & empty_r'image(er);
    report "str=" & string'image("a""b") & " slv=" & std_logic_vector'image(s);
    report "nxt=" & color'image(nc(blue)) & "," & color'image(nc(red)) & " first=" & integer'image(fi((7, 8, 9))) & " max=" & integer'image(maxi(3, 9));
    wait;
  end process;
end architecture;
