// IEEE 1800-2023 38.6 and 38.36.3. vpi_control(vpiReset, stop, reset,
// diagnostics) runs $reset (annex D.8) when the calling routine returns.
// cbStartOfSave / cbEndOfSave run around $save, cbStartOfRestart before
// $restart and cbEndOfRestart once the run is back at the checkpoint,
// cbStartOfReset before $reset and cbEndOfReset after it, before any
// procedure runs. cbSignal (index: the signal) runs at a safe point after
// the signal arrives. msim runs the simulation again for reset and
// restart: run 0 saves at 10 and resets at 20 through VPI; run 1 restarts
// and comes back to time 10 just after the $save.
//! icarus-skip: Icarus has no $reset, $save or cbSignal
//! args: data/vpi/features.c
module top;
  initial begin
    $display("run %0d value %0d", $reset_count, $reset_value);
    if ($reset_count == 1) $restart("spec_38_36_feat.chk");
    #10 $save("spec_38_36_feat.chk");
    $display("saved t=%0t run %0d", $time, $reset_count);
    #5 $poke;
    $display("poked t=%0t", $time);
    #1 $display("t=%0t", $time);
    #4 if ($reset_count == 0) $vreset;
    $display("end run %0d", $reset_count);
  end
endmodule
//> SIGKILL refused
//> run 0 value 0
//> start of save
//> end of save
//> saved t=10 run 0
//> raised
//> poked t=15
//> signal 10 at 16
//> t=16
//> after vpi_control
//> start of reset
//> SIGKILL refused
//> end of reset
//> run 1 value 9
//> start of restart
//> end of restart
//> saved t=10 run 1
//> raised
//> poked t=15
//> signal 10 at 16
//> t=16
//> end run 1
