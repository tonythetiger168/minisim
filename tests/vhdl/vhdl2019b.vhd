-- VHDL-2019 conditional analysis (24.2), sequential blocks (10.15) and
-- conditional return (10.13); MY_MODE comes from +define+. Expected output
-- worked out from the LRM.
entity vhdl2019b_tb is end;
architecture a of vhdl2019b_tb is
  function pick(c : boolean; x, y : integer) return integer is
  begin
    return x when c else y;
  end;
  function sgn(x : integer) return integer is
  begin
    return -1 when x < 0 else 0 when x = 0 else 1;
  end;
begin
  process
    variable n : integer := 0;
  begin
    `if TOOL_TYPE = "SIMULATION" and VHDL_VERSION >= "2019" then
    report "simulation, 2019";
    `elsif TOOL_TYPE = "SYNTHESIS" then
    report "synthesis";
    `else
    report "other";
    `end if
    `if MY_MODE = "fast" then
    report "fast mode";
    `else
    report "default mode";
    `end
    outer: block
      variable k : integer := 4;
    begin
      inner: block
        variable j : integer := 10;
      begin
        n := pick(true, k, 0) + pick(false, 0, j);
      end block inner;
    end block outer;
    report "n=" & integer'image(n) & " sgn=" & integer'image(sgn(-5)) & integer'image(sgn(0)) & integer'image(sgn(7));
    wait;
  end process;
end;
