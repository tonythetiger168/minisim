-- VHDL-2019 std.env GET_CALL_PATH (16.5.7).
use std.env.all;
entity callpath2019_tb is end;
architecture t of callpath2019_tb is
  impure function depth return natural is
    variable p : call_path_vector_ptr;
  begin
    p := get_call_path;
    return p.all'length;
  end;
  procedure inner(tag : string) is
    variable p : call_path_vector_ptr;
  begin
    p := get_call_path;
    report tag & ": " & to_string(p, " | ");
    report "inner: left=" & integer'image(p.all'left) & " name0=" & p(0).name.all & " line0=" & integer'image(p(0).file_line)
           & " root=" & p(p.all'right).name.all;
    deallocate(p);
  end;
  procedure outer is
  begin
    inner("via outer");
  end;
begin
  main: process
    variable p : call_path_vector_ptr;
  begin
    p := get_call_path;
    report "process: " & to_string(p.all(0));
    inner("direct");
    outer;
    report "depth in f: " & integer'image(depth);
    report "element: " & to_string(p(0));
    wait;
  end process;
end;
