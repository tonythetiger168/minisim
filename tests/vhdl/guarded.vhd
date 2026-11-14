-- Guarded signals (1076-2019 6.4.2.3, 11.6, 7.4 disconnection specification)
library ieee;
use ieee.std_logic_1164.all;

entity guarded_tb is
end entity;

architecture sim of guarded_tb is
  subtype nib is std_logic_vector(3 downto 0);
  signal en1, en2 : boolean := false;
  signal b : std_logic bus;                       -- one guarded driver: Z when disconnected
  signal v : nib bus := "0000";     -- two guarded drivers
  signal r : std_logic register := '0';                   -- holds its value when disconnected
  signal r2 : std_logic_vector(1 downto 0) register := "00";
  signal p : std_logic bus;                        -- process driver disconnected with null
  disconnect v : nib after 2 ns;
  disconnect all : std_logic after 0 ns;
begin
  g1 : block (en1) is
  begin
    b <= guarded '1';
    v <= guarded "0011";
    r <= guarded '1';
    r2 <= guarded "10";
  end block;

  g2 : block (en2) is
  begin
    v <= guarded "0101";
    r2 <= guarded "01";
  end block;

  drv : process
  begin
    p <= '1';
    wait for 15 ns;
    p <= null;
    wait;
  end process;

  stim : process
  begin
    wait for 1 ns;
    report "t1 b=" & std_logic'image(b) & " v=" & to_string(v) & " r=" & std_logic'image(r) & " r2=" & to_string(r2) & " p=" & std_logic'image(p);
    en1 <= true;
    wait for 5 ns;
    report "t6 b=" & std_logic'image(b) & " v=" & to_string(v) & " r=" & std_logic'image(r) & " r2=" & to_string(r2);
    en2 <= true;
    wait for 5 ns;
    report "t11 v=" & to_string(v) & " r2=" & to_string(r2);
    en1 <= false;
    wait for 1 ns;
    report "t12 b=" & std_logic'image(b) & " v=" & to_string(v) & " r=" & std_logic'image(r) & " r2=" & to_string(r2);
    wait for 2 ns;
    report "t14 v=" & to_string(v);
    en2 <= false;
    wait for 3 ns;
    report "t17 v=" & to_string(v) & " r2=" & to_string(r2) & " p=" & std_logic'image(p);
    wait;
  end process;
end architecture;
