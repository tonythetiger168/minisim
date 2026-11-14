-- std_logic with its nine values (IEEE 1164): 'U' before the first
-- assignment, the weak values L H W, '-', the logical operators' 'U'
-- propagation, '=' and to_x01, ports between entities, and resolution of
-- weak and strong drivers (pull-ups and pull-downs). Golden made with GHDL.
library ieee;
use ieee.std_logic_1164.all;
entity std9_inv is
  port (a : in std_logic_vector(3 downto 0); y : out std_logic_vector(3 downto 0); p : out std_logic);
end entity;
architecture rtl of std9_inv is
begin
  y <= not a;
  p <= a(3);   -- passes the weak value through
end architecture;

library ieee;
use ieee.std_logic_1164.all;
entity std9_tb is end entity;
architecture sim of std9_tb is
  signal s : std_logic;
  signal v : std_logic_vector(3 downto 0);
  signal w : std_logic_vector(3 downto 0) := "LH01";
  signal c : std_logic_vector(3 downto 0);
  signal pp : std_logic;
  signal sda : std_logic;
  signal bus4 : std_logic_vector(3 downto 0);
  signal d0, d1 : std_logic := 'Z';
  signal sel : std_logic_vector(3 downto 0) := "0000";
  signal clk : std_logic := 'L';
  signal cnt : integer := 0;
begin
  u : entity work.std9_inv port map (a => w, y => c, p => pp);

  -- an open-drain line with a pull-up, and a bus with pull-downs
  sda <= d0;
  sda <= d1;
  sda <= 'H';
  g : for i in 0 to 3 generate
    bus4(i) <= '1' when sel(i) = '1' else 'Z';
  end generate;
  bus4 <= (others => 'L');

  -- rising_edge sees L -> H as an edge (to_x01)
  process (clk)
  begin
    if rising_edge(clk) then cnt <= cnt + 1; end if;
  end process;

  process
    variable x : std_logic;
    variable r : std_logic_vector(3 downto 0);
  begin
    report "s=" & std_logic'image(s) & " v=" & to_string(v) & " w=" & to_string(w) & " x=" & std_logic'image(x);
    wait for 1 ns;
    report "c=" & to_string(c) & " p=" & std_logic'image(pp) & " and=" & to_string(w and "1111") & " notU=" & std_logic'image(not s);
    report "UandO=" & std_logic'image(s and '0') & " UorO=" & std_logic'image(s or '0') & " UorH=" & std_logic'image(s or 'H');
    report "sda=" & std_logic'image(sda) & " bus=" & to_string(bus4);
    s <= 'H';
    v <= "UX-W";
    d0 <= '0'; sel <= "0101";
    clk <= 'H';
    wait for 1 ns;
    report "s=" & std_logic'image(s) & " v=" & to_string(v) & " s=H " & boolean'image(s = 'H') & " s=1 " & boolean'image(s = '1');
    report "v=UX-W " & boolean'image(v = "UX-W") & " v=XX-W " & boolean'image(v = "XX-W") & " x01=" & to_string(to_x01(v));
    report "sda=" & std_logic'image(sda) & " bus=" & to_string(bus4) & " cnt=" & integer'image(cnt);
    x := 'W';
    r := v xor "0101";
    report "x=" & std_logic'image(x) & " r=" & to_string(r) & " ux01=" & std_logic'image(to_ux01(v(3)));
    d0 <= 'L'; d1 <= 'H';
    clk <= 'L';
    wait for 1 ns;
    report "sda=" & std_logic'image(sda) & " x01=" & std_logic'image(to_x01(sda));
    d0 <= 'U';
    clk <= '1';
    wait for 1 ns;
    report "sda=" & std_logic'image(sda) & " cnt=" & integer'image(cnt);
    d0 <= '1'; d1 <= '0';
    wait for 1 ns;
    report "sda=" & std_logic'image(sda) & " eqX " & boolean'image(sda = 'X');
    wait;
  end process;
end architecture;
