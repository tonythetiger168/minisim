-- VHDL models whose std_logic ports sit on supply nets through the VHDL value
-- conversion tables of IEEE 1801-2024 Annex B (B.2 to B.5, B.10, B.12):
-- run with vct_vhdl.sv and vct_vhdl.upf.
library ieee;
use ieee.std_logic_1164.all;
entity ldo is
  port (en : in std_logic; vout, gnd_n : out std_logic);
end entity;
architecture rtl of ldo is
begin
  vout <= en;
  gnd_n <= not en;   -- its ground output is active low: '0' is on
end architecture;
library ieee;
use ieee.std_logic_1164.all;
entity macro is
  port (VDD, VSSN, TIEH, TIEL : in std_logic; a : in std_logic_vector(3 downto 0); y : out std_logic_vector(3 downto 0));
end entity;
architecture rtl of macro is
begin
  y <= not a when VDD = '1' and VSSN = '0' else (others => 'X');
end architecture;
