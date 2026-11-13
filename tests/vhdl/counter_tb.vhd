library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity counter_tb is end;

architecture sim of counter_tb is
  signal clk, rst, en : std_logic := '0';
  signal count : std_logic_vector(3 downto 0);
  type state_t is (idle, running, done);
  signal st : state_t := idle;
begin
  dut : entity work.counter generic map (WIDTH => 4) port map (clk => clk, rst => rst, en => en, count => count);
  clk <= not clk after 5 ns;
  process
    variable n : integer := 0;
  begin
    rst <= '1';
    wait for 12 ns;
    rst <= '0';
    en <= '1';
    st <= running;
    for i in 1 to 20 loop
      wait until rising_edge(clk);
      n := n + 1;
    end loop;
    wait for 1 ns;
    report "count=" & integer'image(to_integer(unsigned(count))) & " n=" & integer'image(n) & " st=" & state_t'image(st);
    assert count = "0100" report "bad count" severity error;
    st <= done;
    wait for 1 ns;
    report "done " & to_string(count) & " " & to_hstring(count);
    std.env.finish;
  end process;
end architecture;
