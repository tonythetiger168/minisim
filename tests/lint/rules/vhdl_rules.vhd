--! args: -top vhdl_rules -enable VHD004 -enable VHD007 -enable VHD009 -disable STY001
-- The VHDL-only rules (src/lintvhdl.hpp), on the VHDL text, and the rules
-- that see VHDL through the translation in the forms VHDL writes them.
library ieee;
use ieee.std_logic_1164.all;
use ieee.std_logic_unsigned.all; --~ VHD001
use ieee.numeric_std.all;

entity vhdl_leaf is
  port (a : in std_logic; y : out std_logic);
end entity;
architecture rtl of vhdl_leaf is
begin
  y <= not a;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity vhdl_rules is
  port (
    clk   : in  std_logic;
    a     : in  std_logic;
    b     : in  std_logic;
    flag  : in  bit; --~ VHD009
    y     : out std_logic;
    z     : out std_logic;
    w     : out std_logic;
    q     : buffer std_logic; --~ VHD003
    n     : out integer; --~ VHD008
    m     : out integer range 0 to 15
  );
end entity;

architecture rtl of vhdl_rules is
  signal r    : std_logic := '0'; --~ VHD004
  signal s    : std_logic;
  signal cnt  : natural; --~ VHD008
  signal c4   : natural range 0 to 15;
  shared variable sv : integer; --~ VHD005 UNU002
begin
  u0 : entity work.vhdl_leaf port map (a, w); --~ VHD006
  u1 : entity work.vhdl_leaf port map (a => b, y => open);

  comb : process (a) --~ COMB001
  begin
    y <= a and b;
  end process;

  edge : process (clk)
  begin
    if clk'event and clk = '1' then --~ VHD002
      r <= a;
    end if;
  end process;

  -- wait until at the top is a flop, not an implicit state machine
  waits : process
  begin
    wait until rising_edge(clk);
    s <= b;
    cnt <= cnt + 1;
    c4 <= c4 + 1;
  end process;

  process (clk) --~ VHD007
  begin
    if rising_edge(clk) then
      q <= not q;
    end if;
  end process;

  z <= r or s or to_stdulogic(flag);
  n <= cnt;
  m <= c4;
end architecture;
