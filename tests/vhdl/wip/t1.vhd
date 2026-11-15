use work.reflection.all;
entity t1_tb is end;
architecture t of t1_tb is
begin
  process
    variable s : subtype_mirror;
    variable v : value_mirror;
    variable e : enumeration_subtype_mirror;
  begin
    s := msim_enum_sm("color", "red" & LF & "green" & LF & "blue", 0, 2, true);
    v := msim_enum_vm(s, 1);
    e := s.to_enumeration;
    report s.simple_name & " " & type_class'image(s.get_type_class) & " " & v.to_enumeration.image & " len=" & integer'image(integer(e.length))
      & " high=" & e.high.image & " lit=" & integer'image(e.enumeration_literal("blue").pos);
    s := msim_int_sm("small", 0, 15, true);
    report integer'image(s.to_integer.high.value) & " " & integer'image(integer(s.to_integer.length));
    wait;
  end process;
end;
