-- waits, procedures with signal parameters, loops with exit/next, signed math, shifts, attributes, textio
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;

entity seq is end;
architecture sim of seq is
  signal clk : std_logic := '0';
  signal d   : std_logic_vector(7 downto 0) := (others => '0');
  signal q   : std_logic_vector(7 downto 0);
  signal i0  : integer;
  signal stop : boolean := false;
  procedure drive(signal s : out std_logic_vector; v : in integer; signal c : in std_logic) is
  begin
    wait until rising_edge(c);
    s <= std_logic_vector(to_unsigned(v, s'length));
  end procedure;
  function sat(x : integer; lo, hi : integer) return integer is
  begin
    if x < lo then return lo; elsif x > hi then return hi; else return x; end if;
  end function;
begin
  clk <= not clk after 5 ns when not stop else '0';
  q <= d when rising_edge(clk);
  process
    variable l : line;
    variable s8 : signed(7 downto 0);
    variable u8 : unsigned(7 downto 0);
    variable k : integer := 0;
    variable r : real := 2.5;
    variable t : time := 3 ns;
    alias hi_nib : std_logic_vector(3 downto 0) is d(7 downto 4);
  begin
    report "i0 default=" & integer'image(i0);
    drive(d, 17, clk);
    wait for 1 ns;
    report "d=" & to_hstring(d) & " q=" & to_hstring(q) & " hi=" & to_string(hi_nib);
    wait until q = x"11";
    report "q=" & to_hstring(q) & " at " & time'image(now);
    s8 := to_signed(-100, 8);
    s8 := s8 - 50;
    report "s8=" & integer'image(to_integer(s8)) & " abs=" & integer'image(to_integer(abs(to_signed(-7, 8))));
    u8 := x"81";
    report "sll=" & to_hstring(u8 sll 1) & " srl=" & to_hstring(u8 srl 3) & " rol=" & to_hstring(u8 rol 1) & " ror=" & to_hstring(u8 ror 1);
    report "shr=" & to_hstring(shift_right(to_signed(-16, 8), 2)) & " shl=" & to_hstring(shift_left(u8, 4)) & " rsz=" & to_hstring(resize(to_signed(-3, 4), 8));
    outer : for i in 0 to 9 loop
      for j in 0 to 9 loop
        next outer when j > i;
        exit outer when i * j > 20;
        k := k + 1;
      end loop;
    end loop;
    report "k=" & integer'image(k);
    while k > 3 loop
      k := k / 2;
    end loop;
    report "k=" & integer'image(k) & " mod=" & integer'image((-7) mod 3) & " rem=" & integer'image((-7) rem 3) & " pow=" & integer'image(2**10);
    report "sat=" & integer'image(sat(99, 0, 15)) & " " & integer'image(sat(-4, 0, 15));
    r := r * 2.0 + real(k);
    report "r=" & integer'image(integer(r)) & " t=" & time'image(t * 4) & " div=" & integer'image(t / 1 ps);
    report "attr " & integer'image(d'left) & " " & integer'image(d'right) & " " & integer'image(d'length) & " " & boolean'image(d'ascending)
      & " " & integer'image(integer'high) & " " & integer'image(natural'low);
    write(l, string'("line: "));
    write(l, 42);
    write(l, ' ');
    write(l, d);
    write(l, string'(" "));
    write(l, true);
    writeline(output, l);
    wait for 3 ns;
    wait on q for 20 ns;
    report "woke at " & time'image(now);
    stop <= true;
    wait;
  end process;
end;
