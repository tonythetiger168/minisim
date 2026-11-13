-- Protected types (VHDL-2008 clause 5.6): a counter and a statistics
-- object shared between processes, with overloaded methods, methods that
-- call each other, string parameters and an unconstrained result.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package stats_pkg is
  type counter_t is protected
    procedure inc;
    procedure inc(n : integer);
    impure function get return integer;
    procedure set_name(s : string);
    impure function name_len return natural;
  end protected counter_t;

  type stats_t is protected
    procedure add(x : integer);
    impure function count return natural;
    impure function mean return real;
    impure function max return integer;
    impure function as_slv(width : natural) return std_logic_vector;
  end protected stats_t;

  shared variable global_count : counter_t;
end package;

package body stats_pkg is
  type counter_t is protected body
    variable value : integer := 0;
    variable nlen : natural := 0;
    procedure inc is
    begin
      value := value + 1;
    end procedure;
    procedure inc(n : integer) is
    begin
      for i in 1 to n loop
        inc;
      end loop;
    end procedure;
    impure function get return integer is
    begin
      return value;
    end function;
    procedure set_name(s : string) is
    begin
      nlen := s'length;
    end procedure;
    impure function name_len return natural is
    begin
      return nlen;
    end function;
  end protected body counter_t;

  type stats_t is protected body
    variable n : natural := 0;
    variable sum : integer := 0;
    variable mx : integer := integer'low;
    procedure add(x : integer) is
    begin
      n := n + 1;
      sum := sum + x;
      if x > mx then
        mx := x;
      end if;
    end procedure;
    impure function count return natural is
    begin
      return n;
    end function;
    impure function mean return real is
    begin
      if n = 0 then
        return 0.0;
      end if;
      return real(sum) / real(n);
    end function;
    impure function max return integer is
    begin
      return mx;
    end function;
    impure function as_slv(width : natural) return std_logic_vector is
    begin
      return std_logic_vector(to_unsigned(sum, width));
    end function;
  end protected body stats_t;
end package body;

library ieee;
use ieee.std_logic_1164.all;
use work.stats_pkg.all;

entity protected_tb is
end entity;

architecture sim of protected_tb is
  shared variable st : stats_t;
  signal done1, done2 : boolean := false;
begin
  p1: process
  begin
    for i in 1 to 5 loop
      st.add(i * 10);
      global_count.inc;
      wait for 1 ns;
    end loop;
    done1 <= true;
    wait;
  end process;

  p2: process
  begin
    wait for 500 ps;
    for i in 1 to 3 loop
      st.add(-i);
      global_count.inc(2);
      wait for 1 ns;
    end loop;
    done2 <= true;
    wait;
  end process;

  check: process
    variable local : counter_t;
  begin
    wait until done1 and done2;
    report "count=" & integer'image(st.count) & " max=" & integer'image(st.max);
    report "mean_x10=" & integer'image(integer(st.mean * 10.0));
    report "slv=" & to_string(st.as_slv(12));
    report "global=" & integer'image(global_count.get);
    local.set_name("protected");
    local.inc(3);
    report "local=" & integer'image(local.get) & " name_len=" & integer'image(local.name_len);
    wait;
  end process;
end architecture;
