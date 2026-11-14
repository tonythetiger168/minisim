-- A package instance declared in an architecture (4.9).
package fifo_g is
  generic (depth : positive; type elem; init : elem);
  type mem_t is array (0 to depth - 1) of elem;
  function fill return mem_t;
end;
package body fifo_g is
  function fill return mem_t is
    variable m : mem_t;
  begin
    for i in m'range loop m(i) := init; end loop;
    return m;
  end;
end;

entity localpkg_tb is end;
architecture t of localpkg_tb is
  package ints is new work.fifo_g generic map (depth => 3, elem => integer, init => 7);
  use ints.all;
  package chars is new work.fifo_g generic map (depth => 2, elem => character, init => 'z');
  signal m : ints.mem_t := ints.fill;
begin
  process
    variable c : chars.mem_t := chars.fill;
  begin
    report "m(2)=" & integer'image(m(2)) & " len=" & integer'image(m'length) & " c=" & c(0) & c(1);
    wait;
  end process;
end;
