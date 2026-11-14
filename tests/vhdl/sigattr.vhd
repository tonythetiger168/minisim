-- Signal attributes (16.2.4): 'last_event, 'delayed, 'transaction,
-- 'stable(T), 'quiet(T).
library ieee; use ieee.std_logic_1164.all;
entity sigattr_tb is end;
architecture t of sigattr_tb is
  signal s : std_ulogic := '0';
  signal v : std_ulogic_vector(3 downto 0) := x"0";
  signal n : integer := 0;
  signal d1, d0 : std_ulogic;
  signal tcount : natural := 0;
  signal st, qt : boolean;
begin
  st <= s'stable(2 ns);
  qt <= v'quiet(1500 ps);
  process begin
    wait on st, qt;
    if now > 0 ns then report "st=" & boolean'image(st) & " qt=" & boolean'image(qt) & " s'last_value=" & std_ulogic'image(s'last_value) & " v'last_value=" & to_string(v'last_value); end if;
  end process;
  d1 <= s'delayed(3 ns);
  d0 <= s'delayed;
  process (v'transaction) begin
    tcount <= tcount + 1;
  end process;
  process begin
    wait on s, d1, d0;
    report "s=" & std_ulogic'image(s) & " d1=" & std_ulogic'image(d1) & " d0=" & std_ulogic'image(d0);
  end process;
  process begin
    wait for 2 ns;
    s <= '1'; v <= x"5"; n <= 7;
    wait for 1 ns;
    report "last_event s=" & time'image(s'last_event) & " v=" & time'image(v'last_event) & " n=" & time'image(n'last_event);
    report "stable(2ns)=" & boolean'image(s'stable(2 ns)) & " stable(0.5ns)=" & boolean'image(s'stable(500 ps)) & " stable=" & boolean'image(s'stable);
    report "quiet(2ns)=" & boolean'image(v'quiet(2 ns)) & " quiet(0.5ns)=" & boolean'image(v'quiet(500 ps));
    wait for 2 ns;
    report "last_event s=" & time'image(s'last_event) & " stable(2ns)=" & boolean'image(s'stable(2 ns));
    v <= x"6"; wait for 1 ns;
    v <= x"6"; wait for 1 ns;
    report "tcount=" & integer'image(tcount);
    s <= '0'; wait for 0 ns;
    report "same delta: last_event=" & time'image(s'last_event) & " stable=" & boolean'image(s'stable(1 ns));
    wait for 10 ns;
    wait;
  end process;
end;
