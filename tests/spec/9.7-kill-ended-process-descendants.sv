// 9.7: kill() on a process that has already finished still terminates its
// descendant subprocesses that are alive. A process that kills itself
// takes its descendants down as well and does not resume.
//! icarus-skip: Icarus 13 has no std::process
module top;
  process f, s;
  bit orphan, selfkid, selfres, survivor;
  initial begin
    fork begin f = process::self(); fork #10 orphan = 1; join_none end join_none
    fork begin s = process::self(); fork #10 selfkid = 1; join_none process::self().kill(); #1 selfres = 1; end join_none
    #1 $display("parent %s", f.status().name());
    f.kill();
    $display("still %s", f.status().name());
    fork begin fork #10 survivor = 1; join_none end join_none
    #20 $display("orphan %0d selfkid %0d selfres %0d survivor %0d", orphan, selfkid, selfres, survivor);
  end
endmodule
//> parent FINISHED
//> still FINISHED
//> orphan 0 selfkid 0 selfres 0 survivor 1
