-- Package instantiation in a block, a process and a subprogram (1076-2019 4.9, 6.1)
package scaler is
  generic (k : integer);
  function scale(x : integer) return integer;
end package;

package body scaler is
  function scale(x : integer) return integer is
  begin
    return x * k;
  end function;
end package body;

entity localpkg2_tb is
end entity;

architecture sim of localpkg2_tb is
  function triple(x : integer) return integer is
    package by3 is new work.scaler generic map (k => 3);
  begin
    return by3.scale(x);
  end function;
  signal s : integer := 0;
begin
  b : block is
    package by5 is new work.scaler generic map (k => 5);
    use by5.all;
  begin
    s <= scale(4);
  end block;

  p : process
    package by7 is new work.scaler generic map (k => 7);
  begin
    wait for 1 ns;
    report "block: " & integer'image(s);
    report "process: " & integer'image(by7.scale(6));
    report "function: " & integer'image(triple(11));
    wait;
  end process;
end architecture;
