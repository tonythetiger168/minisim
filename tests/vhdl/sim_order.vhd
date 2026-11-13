-- Simulation-cycle order (IEEE 1076-2019 14.7.5), constrained strings, dead
-- branches and tables of records with strings, as found in the NEORV32
-- testbench. The golden output is from GHDL.
library ieee;
use ieee.std_logic_1164.all;

entity sim_order_tb is
  generic (G : natural := 2);
end entity;

architecture sim of sim_order_tb is
  constant t_clk : time := (1 sec) / 100000000;
  signal clk_en, clk, rst : std_ulogic;
  signal cfg  : std_ulogic_vector(7 downto 0) := x"5a";
  signal addr : std_ulogic_vector(7 downto 0);
  constant version : std_ulogic_vector(31 downto 0) := x"01130707";

  type entry_t is record
    code : std_ulogic_vector(3 downto 0);
    name : string(1 to 5);
  end record;
  type table_t is array (0 to 2) of entry_t;
  constant table : table_t := (
    (code => "0001", name => "one  "),
    (code => "0010", name => "two  "),
    (code => "0100", name => "four "));

  -- positional elements, then others (9.3.3.3), as in a ROM image
  type rom_t is array (0 to 7) of std_ulogic_vector(7 downto 0);
  constant rom : rom_t := (x"f1", x"40", x"20", others => (others => '0'));
  type down_t is array (3 downto 0) of integer;
  constant down : down_t := (7, 6, others => 1);

  function lookup(c : std_ulogic_vector(3 downto 0)) return string is
  begin
    for i in table'range loop
      if table(i).code = c then
        return table(i).name;
      end if;
    end loop;
    return "none ";
  end function;

  function hex_char(d : std_ulogic_vector(3 downto 0)) return character is
    constant digits : string(1 to 16) := "0123456789abcdef";
    variable n : natural := 0;
  begin
    for i in 3 downto 0 loop
      n := n * 2;
      if d(i) = '1' then n := n + 1; end if;
    end loop;
    return digits(n + 1);
  end function;

  -- a local string(1 to n) starts with n characters, each then assigned
  function hex(d : std_ulogic_vector) return string is
    variable v : string(1 to d'length / 4);
  begin
    for i in 0 to d'length / 4 - 1 loop
      v(i + 1) := hex_char(d(d'high - i * 4 downto d'high - i * 4 - 3));
    end loop;
    return v;
  end function;
begin
  rst    <= '0', '1' after 30 * (t_clk / 2);
  clk_en <= '0', '1' after 60 * (t_clk / 2), '0' after 500 ns;   -- the clock stops, so the run ends
  clk    <= (not clk) and clk_en after t_clk / 2;

  -- a branch that is dead for this generic holds a null-range slice; the
  -- rest of the process still runs
  read_back: process (cfg)
  begin
    addr <= (others => '0');
    addr(7 downto G) <= cfg(7 downto G);
    if G > 2 then
      addr(G - 3 downto 0) <= (others => '1');
    end if;
  end process;

  check: process
  begin
    -- clk's transaction due at 400 ns is applied before this process resumes
    wait for 400 ns;
    report "at 400 ns: clk=" & std_ulogic'image(clk) & " rst=" & std_ulogic'image(rst);
    wait for 2 ns;
    report "at 402 ns: clk=" & std_ulogic'image(clk);
    wait for 3 ns;
    report "at 405 ns: clk=" & std_ulogic'image(clk);
    report "version v" & hex(version(31 downto 24)) & "." & hex(version(23 downto 16)) & "." & hex(version) & " cfg " & hex(cfg) & " addr " & hex(addr);
    report "rom: " & hex(rom(0)) & hex(rom(1)) & hex(rom(2)) & hex(rom(3)) & hex(rom(7)) &
           " down: " & integer'image(down(3)) & integer'image(down(2)) & integer'image(down(1)) & integer'image(down(0));
    report "lookup: " & lookup("0010") & "|" & lookup("0100") & "|" & lookup("1000") & "|";
    wait;
  end process;
end architecture;
