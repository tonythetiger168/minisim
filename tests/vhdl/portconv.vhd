-- Conversions in port maps (6.5.7.3): a function or type conversion on the
-- actual of an in port, and on the formal of an out port.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity cnt is
  port (clk : in std_ulogic; step : in natural; q : out std_ulogic_vector(7 downto 0));
end;
architecture a of cnt is
  signal r : unsigned(7 downto 0) := (others => '0');
begin
  q <= std_ulogic_vector(r);
  process (clk) begin
    if rising_edge(clk) then r <= r + step; end if;
  end process;
end;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
package pc_pkg is
  function to_int(v : std_ulogic_vector) return integer;
end;
package body pc_pkg is
  function to_int(v : std_ulogic_vector) return integer is begin return to_integer(unsigned(v)); end;
end;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
use work.pc_pkg.all;
entity portconv_tb is end;
architecture t of portconv_tb is
  signal clk : std_ulogic := '0';
  signal stepv : unsigned(3 downto 0) := x"3";
  signal n : integer;
  signal u : unsigned(7 downto 0);
begin
  c1: entity work.cnt port map (clk => clk, step => to_integer(stepv), to_int(q) => n);
  c2: entity work.cnt port map (clk => clk, step => 5, unsigned(q) => u);
  process begin
    for i in 1 to 4 loop
      clk <= '1'; wait for 1 ns; clk <= '0'; wait for 1 ns;
      report "n=" & integer'image(n) & " u=" & to_string(u);
    end loop;
    wait;
  end process;
end;
