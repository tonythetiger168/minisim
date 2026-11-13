-- components, generate, conditional and selected assignments, tristate bus, process(all)
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity full_add is
  port (a, b, ci : in std_logic; s, co : out std_logic);
end;
architecture rtl of full_add is
begin
  s  <= a xor b xor ci;
  co <= (a and b) or (ci and (a xor b));
end;

library ieee;
use ieee.std_logic_1164.all;
entity rca is
  generic (N : positive := 4);
  port (a, b : in std_logic_vector(N-1 downto 0); ci : in std_logic;
        s : out std_logic_vector(N-1 downto 0); co : out std_logic);
end;
architecture rtl of rca is
  component full_add is
    port (a, b, ci : in std_logic; s, co : out std_logic);
  end component;
  signal c : std_logic_vector(N downto 0);
begin
  c(0) <= ci;
  g : for i in 0 to N-1 generate
    fa : full_add port map (a => a(i), b => b(i), ci => c(i), s => s(i), co => c(i+1));
  end generate;
  co <= c(N);
end;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
entity structure is end;
architecture sim of structure is
  signal a, b, s : std_logic_vector(7 downto 0);
  signal ci, co : std_logic := '0';
  signal sel : std_logic_vector(1 downto 0) := "00";
  signal y, z : std_logic_vector(3 downto 0);
  signal bus_l : std_logic_vector(3 downto 0);
  signal en1, en2 : std_logic := '0';
  signal cnt : integer;
  signal m : unsigned(7 downto 0);
  constant USE_A : boolean := true;
begin
  u0 : entity work.rca generic map (N => 8) port map (a, b, ci, s, co);
  y <= "0001" when sel = "00" else
       "0010" when sel = "01" else
       "0100" when sel = "10" else
       "1000";
  with sel select z <=
    x"A" when "00",
    x"B" when "01" | "10",
    x"F" when others;
  bus_l <= x"5" when en1 = '1' else (others => 'Z');
  bus_l <= x"A" when en2 = '1' else (others => 'Z');
  gen_a : if USE_A generate
    m <= unsigned(a) + unsigned(b);
  else generate
    m <= unsigned(a) - unsigned(b);
  end generate;
  comb : process (all)
    variable n : integer;
  begin
    n := 0;
    for i in s'range loop
      if s(i) = '1' then n := n + 1; end if;
    end loop;
    cnt <= n;
  end process;
  process
  begin
    a <= x"3C"; b <= x"45"; ci <= '1';
    wait for 1 ns;
    report "s=" & to_hstring(s) & " co=" & std_logic'image(co) & " m=" & integer'image(to_integer(m));
    a <= x"FF"; b <= x"01"; ci <= '0';
    wait for 1 ns;
    report "s=" & to_hstring(s) & " co=" & std_logic'image(co);
    for i in 0 to 3 loop
      sel <= std_logic_vector(to_unsigned(i, 2));
      wait for 1 ns;
      report "sel=" & to_string(sel) & " y=" & to_string(y) & " z=" & to_hstring(z);
    end loop;
    report "bus=" & to_string(bus_l);
    en1 <= '1'; wait for 1 ns;
    report "bus=" & to_string(bus_l);
    en2 <= '1'; wait for 1 ns;
    report "bus=" & to_string(bus_l);
    en1 <= '0'; wait for 1 ns;
    report "bus=" & to_string(bus_l);
    a <= x"07"; b <= x"00"; ci <= '0';
    wait for 1 ns;
    report "ones(s)=" & integer'image(cnt);
    wait;
  end process;
end;
