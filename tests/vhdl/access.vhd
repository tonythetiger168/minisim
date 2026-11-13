-- Access types (VHDL-2008 clause 5.4): a linked list of records with an
-- incomplete type, a growable integer array, access to a scalar, null,
-- deallocate, and the enumeration literals / functions that share a name.
package list_pkg is
  type node_t;
  type node_ptr is access node_t;
  type node_t is record
    value : integer;
    next_node : node_ptr;
  end record;
  type int_array is array (integer range <>) of integer;
  type int_array_ptr is access int_array;
  type int_ptr is access integer;
  type mode_t is (FIRST, LAST, SUM);

  procedure push(variable head : inout node_ptr; v : integer);
  procedure sum_list(variable head : in node_ptr; variable s : out integer);
  function SUM(a : int_array) return integer;
end package;

package body list_pkg is
  procedure push(variable head : inout node_ptr; v : integer) is
    variable n : node_ptr;
  begin
    n := new node_t'(value => v, next_node => head);
    head := n;
  end procedure;

  procedure sum_list(variable head : in node_ptr; variable s : out integer) is
    variable p : node_ptr := head;
  begin
    s := 0;
    while p /= null loop
      s := s + p.value;
      p := p.next_node;
    end loop;
  end procedure;

  function SUM(a : int_array) return integer is
    variable s : integer := 0;
  begin
    for i in a'range loop
      s := s + a(i);
    end loop;
    return s;
  end function;
end package body;

use work.list_pkg.all;

entity access_tb is
end entity;

architecture sim of access_tb is
begin
  process
    variable head : node_ptr := null;
    variable arr, old : int_array_ptr;
    variable ip : int_ptr;
    variable m : mode_t := SUM;
    variable total : integer;
  begin
    for i in 1 to 4 loop
      push(head, i * i);
    end loop;
    sum_list(head, total);
    report "list sum=" & integer'image(total) & " first=" & integer'image(head.value)
         & " second=" & integer'image(head.next_node.value);
    head.next_node.value := 100;
    sum_list(head, total);
    report "after write=" & integer'image(total);

    arr := new int_array(1 to 3);
    for i in arr'range loop
      arr(i) := i * 10;
    end loop;
    report "arr length=" & integer'image(arr'length) & " left=" & integer'image(arr'left)
         & " right=" & integer'image(arr'right) & " a(2)=" & integer'image(arr(2));
    -- grow the array, keeping its elements
    old := arr;
    arr := new int_array(1 to 6);
    arr.all(1 to 3) := old.all(1 to 3);
    deallocate(old);
    arr(6) := 7;
    report "grown length=" & integer'image(arr'length) & " a(3)=" & integer'image(arr(3)) & " a(6)=" & integer'image(arr(6))
         & " old null=" & boolean'image(old = null);
    arr := new int_array'(1 => 5, 2 => 6, 3 => 7);
    report "from value length=" & integer'image(arr'length) & " a(1)=" & integer'image(arr(1))
         & " sum=" & integer'image(SUM(arr.all));

    ip := new integer'(41);
    ip.all := ip.all + 1;
    report "int=" & integer'image(ip.all);

    case m is
      when FIRST => total := 1;
      when LAST => total := 2;
      when SUM => total := 3;
    end case;
    report "mode=" & mode_t'image(m) & " total=" & integer'image(total);
    wait;
  end process;
end architecture;
