// IEEE 1800-2023 Annex D.7-D.9. $log copies the standard output to a file
// and $nolog stops the copy. $reset goes back to time 0; $reset_count and
// $reset_value carry over. $save writes a checkpoint; $restart goes back
// to it, and the run carries on from there. msim runs the simulation
// again for both: run 0 saves at 10 and resets at 20; run 1 restarts at 0,
// which brings it back to time 10 just after the $save.
//! icarus-skip: Icarus has no Annex D tasks
module top;
  integer k = 0;
  initial begin
    if ($reset_count == 1) $restart("spec_d_8.chk");
    $log("spec_d_8.log");
    $display("run %0d value %0d", $reset_count, $reset_value);
    $nolog;
    #10 k = 7;
    $save("spec_d_8.chk");
    $display("after save k=%0d t=%0t run %0d", k, $time, $reset_count);
    #10 k = k + 1;
    $display("k=%0d t=%0t", k, $time);
    if ($reset_count == 0) $reset(1, 42);
    $log;
    $display("logged again");
    $nolog;
    $display("log has %0d lines", lines("spec_d_8.log"));
  end
  function automatic int lines(string fn);
    int fd = $fopen(fn, "r"), n = 0;
    string s;
    while ($fgets(s, fd)) n++;
    $fclose(fd);
    return n;
  endfunction
endmodule
//> run 0 value 0
//> after save k=7 t=10 run 0
//> k=8 t=20
//> after save k=7 t=10 run 1
//> k=8 t=20
//> logged again
//> log has 2 lines
