-- VHDL-2019 'REFLECT and std.reflection (16.2.2, 16.12): subtype and value
-- mirrors of enumerations, std_logic (nine values), integers, reals, time,
-- records, one- and two-dimensional arrays, strings, access values (a linked
-- list), files and protected types. GHDL 4.1 has no VHDL-2019; the golden is
-- checked by hand.
library ieee;
use ieee.std_logic_1164.all;
use std.reflection.all;
entity reflect2019_tb is end;
architecture t of reflect2019_tb is
  type color is (red, green, blue);
  subtype small is integer range 0 to 15;
  type rec is record
    c : color;
    n : small;
    x : real;
    t : time;
  end record;
  type arr is array (2 downto 0) of std_logic;
  signal s : std_logic_vector(3 downto 0) := "01ZH";
  type node;
  type node_ptr is access node;
  type node is record
    v : integer;
    nxt : node_ptr;
  end record;
  type matrix is array (1 to 2, 0 to 2) of integer;
  type int_file is file of integer;
  type counter is protected
    procedure inc;
  end protected;
  type counter is protected body
    variable n : integer := 0;
    procedure inc is begin n := n + 1; end;
  end protected body;
  type state is (idle, run, done);
  subtype busy is state range run to done;
  procedure show(m : value_mirror) is
  begin
    report "class " & value_class'image(m.get_value_class) & " of " & m.get_subtype_mirror.simple_name;
  end;
begin
  process
    variable r : rec := (blue, 7, 2.5, 3 ns);
    variable a : arr := "1L0";
    variable vm : value_mirror;
    variable sm : subtype_mirror;
    variable rv : record_value_mirror;
    variable av : array_value_mirror;
    variable p : node_ptr := new node'(1, new node'(2, null));
    variable m : matrix := ((1, 2, 3), (4, 5, 6));
    variable str : string(1 to 3) := "abc";
    variable b : busy := done;
    variable i : integer := -5;
    variable am : array_subtype_mirror;
  begin
    sm := color'reflect;
    report sm.simple_name & " " & type_class'image(sm.get_type_class) & " " & integer'image(integer(sm.to_enumeration.length));
    vm := r'reflect;
    rv := vm.to_record;
    report "rec " & integer'image(integer(rv.get_subtype_mirror.length)) & " " & rv.get_subtype_mirror.element_name(1)
      & " c=" & rv.get(0).to_enumeration.image & " n=" & integer'image(rv.get(1).to_integer.value)
      & " x=" & real'image(rv.get(2).to_floating.value) & " t=" & rv.get(3).to_physical.image;
    vm := a'reflect;
    av := vm.to_array;
    report "arr len=" & integer'image(integer(av.get_subtype_mirror.length)) & " left=" & integer'image(integer(av.get_subtype_mirror.left))
      & " e2=" & av.get(2).to_enumeration.image & " e1=" & av.get(1).to_enumeration.image & " e0=" & av.get(0).to_enumeration.image;
    wait for 1 ns;
    vm := s'reflect;
    av := vm.to_array;
    report "sig " & av.get(3).to_enumeration.image & av.get(2).to_enumeration.image & av.get(1).to_enumeration.image & av.get(0).to_enumeration.image;
    vm := p'reflect;
    show(vm);
    report "p.v=" & integer'image(vm.to_access.get.to_record.get(0).to_integer.value)
      & " null? " & boolean'image(vm.to_access.is_null)
      & " next.v=" & integer'image(vm.to_access.get.to_record.get(1).to_access.get.to_record.get(0).to_integer.value)
      & " end? " & boolean'image(vm.to_access.get.to_record.get(1).to_access.get.to_record.get(1).to_access.is_null);
    vm := m'reflect;
    am := vm.to_array.get_subtype_mirror;
    report "dims=" & integer'image(integer(am.dimensions)) & " len2=" & integer'image(integer(am.length(2)))
      & " m(2,1)=" & integer'image(vm.to_array.get(2, 1).to_integer.value);
    vm := str'reflect;
    report "str(2)=" & vm.to_array.get(2).to_enumeration.image & " len=" & integer'image(integer(vm.to_array.get_subtype_mirror.length));
    vm := b'reflect;
    report "b=" & vm.to_enumeration.image & " pos=" & integer'image(vm.to_enumeration.pos)
      & " low=" & vm.to_enumeration.get_subtype_mirror.low.image;
    vm := i'reflect;
    report "i=" & integer'image(vm.to_integer.value) & " " & vm.to_integer.image;
    sm := integer'reflect;
    report "integer high=" & integer'image(sm.to_integer.high.value);
    sm := time'reflect;
    report "time units=" & integer'image(integer(sm.to_physical.units_length)) & " " & sm.to_physical.unit_name(3)
      & " scale(ns)=" & integer'image(sm.to_physical.scale("ns"));
    sm := counter'reflect;
    report "prot " & type_class'image(sm.get_type_class) & " " & sm.simple_name;
    sm := int_file'reflect;
    report "file " & type_class'image(sm.get_type_class) & " of " & sm.to_file.designated_subtype.simple_name;
    wait;
  end process;
end;
