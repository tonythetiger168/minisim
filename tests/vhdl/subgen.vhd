-- Generic subprograms and subprogram instantiation (4.2.1, 4.4). GHDL 4.1
-- cannot analyze these, so the expected output was worked out from the LRM.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
package sg_pkg is
  function rot(v : unsigned) return unsigned;
end;
package body sg_pkg is
  function rot(v : unsigned) return unsigned is begin return v(v'high - 1 downto 0) & v(v'high); end;
end;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use work.sg_pkg.all;
entity subgen_tb is end;
architecture a of subgen_tb is
  function swap_g generic (type et) parameter (a : et; b : et; sel : boolean) return et is
    variable r : et;
  begin
    if sel then r := b; else r := a; end if;
    return r;
  end;
  procedure show_g generic (type st; function img(x : st) return string is <>)
    parameter (x : st) is
  begin
    report "v=" & img(x);
  end;
  function img(x : boolean) return string is begin return boolean'image(x); end;
  function pick_int is new swap_g generic map (et => integer);
  function pick_u4 is new swap_g generic map (et => unsigned(3 downto 0));
  function img_int(x : integer) return string is begin return integer'image(x); end;
  procedure show_int is new show_g generic map (st => integer, img => img_int);
  procedure show_bool is new show_g generic map (st => boolean, img => boolean'image);
begin
  process
    variable u : unsigned(3 downto 0) := "0011";
  begin
    show_int(pick_int(3, 9, false));
    show_int(pick_int(3, 9, true));
    u := pick_u4(u, rot(u), true);
    report "u=" & to_string(u);
    show_bool(u = "0110");
    wait;
  end process;
end;
