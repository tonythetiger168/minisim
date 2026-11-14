-- Conversions on an inout port's formal and actual (6.5.7.3).
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity pad is
  port (en : in std_logic; d : in unsigned(3 downto 0); io : inout unsigned(3 downto 0); q : out unsigned(3 downto 0));
end;
architecture a of pad is
begin
  io <= d when en = '1' else (others => 'Z');
  q <= io;
end;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity ioconv_tb is end;
architecture t of ioconv_tb is
  signal en : std_logic := '0';
  signal d : unsigned(3 downto 0) := x"5";
  signal bus_s : std_logic_vector(3 downto 0);
  signal q : unsigned(3 downto 0);
begin
  u1: entity work.pad port map (en => en, d => d, std_logic_vector(io) => unsigned(bus_s), q => q);
  bus_s <= "1100" when en = '0' else (others => 'Z');
  process begin
    wait for 1 ns;
    report "en=0 bus=" & to_string(bus_s) & " q=" & to_string(q);
    en <= '1'; wait for 1 ns;
    report "en=1 bus=" & to_string(bus_s) & " q=" & to_string(q);
    wait;
  end process;
end;
