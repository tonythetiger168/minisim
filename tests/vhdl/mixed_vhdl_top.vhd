-- VHDL testbench instantiating a SystemVerilog module (mixed_adder.sv)
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
entity mixed_vhdl_top is end;
architecture sim of mixed_vhdl_top is
  component Adder is
    generic (W : integer := 8);
    port (A, B : in std_logic_vector(W-1 downto 0); Sum : out std_logic_vector(W downto 0));
  end component;
  signal a, b : std_logic_vector(3 downto 0);
  signal s : std_logic_vector(4 downto 0);
begin
  u : Adder generic map (W => 4) port map (A => a, B => b, Sum => s);
  process
  begin
    a <= x"9"; b <= x"8";
    wait for 1 ns;
    report "sum=" & integer'image(to_integer(unsigned(s)));
    wait;
  end process;
end;
