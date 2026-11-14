-- Block headers and guarded blocks (11.2, 11.6): generics and ports of a
-- block with their maps, the implicit GUARD signal, guarded assignments.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity blockhdr_tb is end;
architecture t of blockhdr_tb is
  signal clk, en : std_ulogic := '0';
  signal d : std_ulogic_vector(3 downto 0) := x"0";
  signal q, q2 : std_ulogic_vector(3 downto 0) := x"0";
  signal sum : std_ulogic_vector(3 downto 0);
begin
  b1: block
    generic (w : natural; inv : boolean := false);
    generic map (w => 4);
    port (a : in std_ulogic_vector(w - 1 downto 0); y : out std_ulogic_vector(w - 1 downto 0));
    port map (a => d, y => sum);
  begin
    y <= not a when inv else a(w - 2 downto 0) & a(w - 1);
  end block;

  latch: block (clk = '1' and en = '1') begin
    q <= guarded d;
    q2 <= guarded d when d(0) = '1' else x"f";
  end block;

  process begin
    for i in 1 to 6 loop
      d <= std_ulogic_vector(to_unsigned(i * 3 mod 16, 4));
      en <= '1' when i mod 2 = 0 else '0';
      wait for 1 ns;
      clk <= '1'; wait for 1 ns; clk <= '0'; wait for 1 ns;
      report "d=" & to_string(d) & " sum=" & to_string(sum) & " q=" & to_string(q) & " q2=" & to_string(q2);
    end loop;
    wait;
  end process;
end;
