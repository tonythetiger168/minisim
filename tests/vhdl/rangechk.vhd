-- Range checks (5.2.1, 14.7.4): assigning an integer out of its subtype's
-- range stops the run.
entity rangechk_tb is end;
architecture t of rangechk_tb is
  subtype small is integer range 0 to 15;
  signal s : small := 3;
begin
  process
    variable v : natural := 0;
    variable w : small;
  begin
    v := 5; w := v + 10;
    report "w=" & integer'image(w);
    s <= w;
    wait for 1 ns;
    w := w + 1;
    report "not reached";
    wait;
  end process;
end;
