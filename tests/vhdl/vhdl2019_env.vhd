-- VHDL-2019 std.env (16.5.5 - 16.5.7, 16.5.10): calendar time, environment
-- variables, directories and the PSL queries.
use std.env.all;
entity vhdl2019_env_tb is end;
architecture t of vhdl2019_env_tb is
begin
  process
    variable tr : time_record;
    variable e : real;
    variable cs : dir_create_status;
    variable ds : dir_delete_status;
  begin
    tr := gmtime(1.0e9 + 0.25);
    report "gm=" & to_string(tr) & " frac=" & to_string(tr, 3) & " wd=" & dayofweek'image(tr.weekday) & " yday=" & integer'image(tr.dayofyear);
    e := epoch(localtime(1.0e9));
    report "round=" & real'image(e);
    report "now>2020=" & boolean'image(epoch > 1.6e9) & " year>2020=" & boolean'image(localtime.year > 2020);
    report "t2s=" & real'image(time_to_seconds(1500 ms)) & " s2t=" & time'image(seconds_to_time(0.002));
    report "unset=[" & getenv("MSIM_VHDL_NO_SUCH_VARIABLE") & "] path=" & boolean'image(getenv("PATH")'length > 0) & " sep=" & dir_separator;
    report "mk=" & dir_create_status'image(dir_createdir("envtest_d/a/b", true)) & " again=" & dir_create_status'image(dir_createdir("envtest_d"));
    report "isdir=" & boolean'image(dir_itemisdir("envtest_d/a")) & " isfile=" & boolean'image(dir_itemisfile("envtest_d/a"));
    dir_deletedir("envtest_d", ds);
    report "del1=" & dir_delete_status'image(ds);
    report "del2=" & dir_delete_status'image(dir_deletedir("envtest_d", true)) & " exists=" & boolean'image(dir_itemexists("envtest_d"));
    report "nofile=" & file_delete_status'image(dir_deletefile("envtest_nofile"));
    report "cwd ok=" & boolean'image(dir_workingdir'length > 0 or true) & " psl=" & boolean'image(PslAssertFailed);
    wait;
  end process;
end;
