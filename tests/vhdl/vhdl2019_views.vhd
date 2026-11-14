-- VHDL-2019 mode views (6.5.2): a view as an element's mode, array mode
-- views, 'converse in a port.
library ieee;
use ieee.std_logic_1164.all;

package bus_pkg is
  type req_t is record
    valid : std_logic;
    data  : std_logic_vector(7 downto 0);
    ready : std_logic;
  end record;
  view req_m of req_t is
    valid, data : out;
    ready : in;
  end view;
  type link_t is record
    req : req_t;
    id  : integer;
    irq : std_logic;
  end record;
  view link_m of link_t is
    req : view req_m;
    id : out;
    irq : in;
  end view;
  type req_arr is array (0 to 1) of req_t;
end package;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.bus_pkg.all;
entity master is
  port (clk : in std_logic; l : view link_m; r : view (req_m) of req_arr);
end entity;
architecture a of master is
begin
  process (clk)
    variable cnt : integer := 0;
  begin
    if rising_edge(clk) then
      cnt := cnt + 1;
      l.req.valid <= '1';
      l.req.data <= std_logic_vector(to_unsigned(64 + cnt, 8));
      l.id <= cnt;
      for i in 0 to 1 loop
        r(i).valid <= l.irq;
        r(i).data <= std_logic_vector(to_unsigned(i + 16 * cnt, 8));
      end loop;
    end if;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.bus_pkg.all;
entity slave is
  port (clk : in std_logic; s : view req_m'converse);
end entity;
architecture a of slave is
begin
  s.ready <= s.valid;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use work.bus_pkg.all;
entity vhdl2019_views_tb is
end entity;
architecture t of vhdl2019_views_tb is
  signal clk : std_logic := '0';
  signal l : link_t;
  signal r : req_arr;
begin
  m : entity work.master port map (clk => clk, l => l, r => r);
  s0 : entity work.slave port map (clk => clk, s => l.req);
  process
  begin
    l.irq <= '1';
    r(0).ready <= '0';
    r(1).ready <= '0';
    for k in 1 to 3 loop
      clk <= '1'; wait for 5 ns;
      clk <= '0'; wait for 5 ns;
      report "k=" & integer'image(k) & " valid=" & std_logic'image(l.req.valid) & " ready=" & std_logic'image(l.req.ready)
        & " data=" & to_hstring(l.req.data) & " id=" & integer'image(l.id) & " r0=" & to_hstring(r(0).data) & " r1=" & to_hstring(r(1).data) & " rv=" & std_logic'image(r(1).valid);
    end loop;
    wait;
  end process;
end architecture;
