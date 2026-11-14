-- VHDL-2019 std.env report and assert API (16.5.9): counts, enables, formats.
use std.env.all;
entity vhdl2019_assert_tb is end;
architecture t of vhdl2019_assert_tb is
begin
  process
    variable ok : boolean;
  begin
    report "n0=" & integer'image(GetVhdlAssertCount) & " f=" & boolean'image(IsVhdlAssertFailed);
    assert false report "e1" severity error;
    report "w1" severity warning;
    report "n1=" & integer'image(GetVhdlAssertCount) & " err=" & integer'image(GetVhdlAssertCount(error)) & " fe=" & boolean'image(IsVhdlAssertFailed(error)) & " ff=" & boolean'image(IsVhdlAssertFailed(failure));
    SetVhdlAssertEnable(warning, false);
    report "w2 hidden" severity warning;
    report "en w=" & boolean'image(GetVhdlAssertEnable(warning)) & " e=" & boolean'image(GetVhdlAssertEnable(error)) & " n2=" & integer'image(GetVhdlAssertCount);
    SetVhdlAssertEnable(true);
    ClearVhdlAssert;
    report "n3=" & integer'image(GetVhdlAssertCount) & " f=" & boolean'image(IsVhdlAssertFailed);
    SetVhdlAssertFormat(note, "** {S}: {r:*<8}| at {t.ps} [{s:^9}] {i} {{x}");
    wait for 3 ns;
    report "hello";
    SetVhdlAssertFormat(note, "{q}", ok);
    report "fmt=" & GetVhdlAssertFormat(note) & " valid=" & boolean'image(ok);
    wait;
  end process;
end;
