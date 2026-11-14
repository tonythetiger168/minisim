-- Generic types and subprograms on entities (6.5.6.1, 6.5.6.3 of 1076-2019
-- interface type and subprogram declarations): one copy of the module per
-- set of actuals.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
package gt_pkg is
  type color is (red, green, blue);
  function nxt(c : color) return color;
  function nxt(v : unsigned) return unsigned;
  function img(c : color) return string;
  function img(v : unsigned) return string;
end;
package body gt_pkg is
  function nxt(c : color) return color is begin
    if c = color'high then return color'low; else return color'succ(c); end if;
  end;
  function nxt(v : unsigned) return unsigned is begin return v + 3; end;
  function img(c : color) return string is begin return color'image(c); end;
  function img(v : unsigned) return string is begin return to_string(v); end;
end;

library ieee; use ieee.std_logic_1164.all;
entity reg_g is
  generic (type t; init : t;
           function step(x : t) return t;
           function show(x : t) return string;
           verbose : boolean := false);
  port (clk : in std_ulogic; q : out t);
end;
architecture a of reg_g is
  signal r : t := init;
begin
  q <= r;
  process (clk) begin
    if rising_edge(clk) then
      r <= step(r);
      if verbose then report "reg_g: " & show(r); end if;
    end if;
  end process;
end;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use work.gt_pkg.all;
entity gentype_tb is end;
architecture t of gentype_tb is
  signal clk : std_ulogic := '0';
  signal c : color;
  signal u : unsigned(3 downto 0);
  signal u2 : unsigned(7 downto 0);
begin
  r1: entity work.reg_g generic map (t => color, init => green, step => nxt, show => img) port map (clk, c);
  r2: entity work.reg_g generic map (t => unsigned(3 downto 0), init => "0001", step => nxt, show => img, verbose => true) port map (clk, u);
  r3: entity work.reg_g generic map (unsigned(7 downto 0), x"f0", nxt, img) port map (clk, u2);
  process begin
    for i in 1 to 3 loop
      clk <= '1'; wait for 1 ns; clk <= '0'; wait for 1 ns;
      report "c=" & color'image(c) & " u=" & to_string(u) & " u2=" & to_hstring(u2);
    end loop;
    wait;
  end process;
end;
