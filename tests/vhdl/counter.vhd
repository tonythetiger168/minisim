library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity vhdl_counter is
  port (clk, rst : in std_logic; cnt : out std_logic_vector(3 downto 0));
end entity;

architecture rtl of vhdl_counter is
  signal q : unsigned(3 downto 0);
begin
  process(clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then q <= (others => '0');
      else q <= q + 1;
      end if;
    end if;
  end process;
  cnt <= std_logic_vector(q);
end architecture;
