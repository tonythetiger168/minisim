-- VHDL-2019 std.env directory API (16.5.5): DIR_OPEN, DIR_CLOSE, LINE_VECTOR.
use std.env.all;
use std.textio.all;
entity dir2019_tb is end;
architecture t of dir2019_tb is
begin
  process
    variable d : directory;
    variable st : dir_open_status;
    variable cs : dir_create_status;
    variable lv : line_vector(0 to 1);
    file f : text;
  begin
    dir_createdir("dtest", cs);
    dir_createdir("dtest/sub", cs);
    file_open(f, "dtest/b.txt", write_mode); file_close(f);
    file_open(f, "dtest/a.txt", write_mode); file_close(f);
    dir_open(d, "dtest", st);
    report "status=" & dir_open_status'image(st) & " count=" & integer'image(d.items'length);
    for i in d.items'range loop
      report integer'image(i) & ": " & d.items(i).all;
    end loop;
    report "name ends with dtest: " & boolean'image(d.name.all(d.name'high - 4 to d.name'high) = "dtest");
    dir_close(d);
    report "closed items null: " & boolean'image(d.items = null);
    st := dir_open(d, "nothere");
    report "missing: " & dir_open_status'image(st);
    dir_open(d, "dtest/a.txt", st);
    report "file: " & dir_open_status'image(st);
    lv(0) := new string'("x");
    lv(1) := new string'("yz");
    report "line_vector " & lv(0).all & lv(1).all;
    wait;
  end process;
end;
