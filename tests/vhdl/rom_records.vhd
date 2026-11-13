-- a constant ROM table read with a run-time index, record signals driven
-- field by field from several processes, value-specialized functions
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity rom_records is end;
architecture sim of rom_records is
  type rom_t is array (0 to 9) of std_ulogic_vector(7 downto 0);
  constant ROM : rom_t := (x"11", x"22", x"33", x"44", x"55", x"66", x"77", x"88", x"99", x"AA");
  type ctrl_t is record
    run  : std_ulogic;
    halt : std_ulogic;
  end record;
  signal ctrl : ctrl_t;
  signal addr : integer range 0 to 9 := 0;
  signal data : std_ulogic_vector(7 downto 0);
  type rsp_arr_t is array (0 to 1) of ctrl_t;
  signal rsp : rsp_arr_t;
  signal seen : integer := 0;
  function replicate(d : std_ulogic; n : natural) return std_ulogic_vector is
    variable v : std_ulogic_vector(n-1 downto 0);
  begin
    v := (others => d);
    return v;
  end function;
begin
  data <= ROM(addr);
  ctrl.run <= '1' when addr > 3 else '0';
  process (addr)
  begin
    ctrl.halt <= '1' when addr = 9 else '0';
  end process;
  rsp(0) <= ctrl;
  rsp(1) <= (run => ctrl.halt, halt => ctrl.run);
  process (rsp)
  begin
    seen <= seen + 1;
  end process;
  process
  begin
    for i in 0 to 9 loop
      addr <= i;
      wait for 1 ns;
      report integer'image(i) & ": " & to_hstring(data) & " run=" & std_ulogic'image(ctrl.run) & " halt=" & std_ulogic'image(ctrl.halt)
        & " rsp1=" & std_ulogic'image(rsp(1).run);
    end loop;
    report "rep=" & to_string(replicate('1', 5)) & " " & to_string(replicate('0', 3)) & " seen>0=" & boolean'image(seen > 0);
    wait;
  end process;
end;
