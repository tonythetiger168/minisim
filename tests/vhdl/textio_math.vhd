library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all; use std.textio.all;
use ieee.math_real.all;
entity textio_math is end;
architecture a of textio_math is
begin
process
  file fin : text open read_mode is "textio_math.txt";
  variable l : line;
  variable a, b : integer;
  variable v : std_logic_vector(3 downto 0);
  variable h : std_logic_vector(7 downto 0);
  variable s1, s2 : positive := 7;
  variable x : real;
  variable sel : std_logic_vector(2 downto 0) := "101";
begin
  readline(fin, l); read(l, a); read(l, b);
  report "a+b=" & integer'image(a+b);
  readline(fin, l); read(l, v); hread(l, h);
  report "v=" & to_string(v) & " h=" & to_hstring(h) & " eof=" & boolean'image(endfile(fin));
  uniform(s1, s2, x);
  report "u=" & integer'image(integer(x * 1000.0)) & " clog=" & integer'image(integer(ceil(log2(real(100)))));
  case? sel is
    when "1-1" => report "match";
    when others => report "nomatch";
  end case?;
  assert false report "fatal one" severity failure;
  report "not here";
  wait;
end process;
end;
