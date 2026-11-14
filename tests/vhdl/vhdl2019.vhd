-- VHDL-2019 additions: mode views and 'converse (6.5.2), conditional
-- analysis (24.2), sequential blocks (10.15), conditional return (10.13).
-- GHDL 4.1 ships no 2019 libraries, so the expected output was worked out
-- from the LRM.
library ieee; use ieee.std_logic_1164.all;
package bus_pkg is
  type bus_t is record
    req  : std_ulogic;
    data : std_ulogic_vector(7 downto 0);
    ack  : std_ulogic;
  end record;
  view master of bus_t is
    req, data : out;
    ack : in;
  end view;
  alias slave is master'converse;
end;

library ieee; use ieee.std_logic_1164.all;
use work.bus_pkg.all;
entity dev is
  port (clk : in std_ulogic; b : view slave of bus_t);
end;
architecture a of dev is begin
  process (clk) begin
    if rising_edge(clk) then b.ack <= b.req; end if;
  end process;
end;

library ieee; use ieee.std_logic_1164.all;
use work.bus_pkg.all;
entity vhdl2019_tb is end;
architecture a of vhdl2019_tb is
  signal clk : std_ulogic := '0';
  signal b : bus_t := ('0', x"00", '0');
  function pick(c : boolean; x, y : integer) return integer is
  begin
    return x when c else y;
  end;
begin
  d: entity work.dev port map (clk, b);
  process
    variable n : integer;
  begin
    `if TOOL_TYPE = "SIMULATION" then
    report "simulation tool";
    `else
    report "other tool";
    `end if
    blk: block
      variable k : integer := 4;
    begin
      n := pick(true, k, 0) + pick(false, 0, 1);
    end block;
    report "n=" & integer'image(n);
    b.req <= '1'; b.data <= x"5a";
    clk <= '1'; wait for 1 ns; clk <= '0'; wait for 1 ns;
    report "ack=" & std_ulogic'image(b.ack) & " data=" & to_hstring(b.data);
    wait;
  end process;
end;
