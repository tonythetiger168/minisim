// IEEE 1800-2023 9.7: process status through a life (RUNNING, WAITING,
// SUSPENDED, FINISHED), await, suspend and resume, kill of a process and
// its children, and self in a class method.
//! icarus-skip: Icarus 13 does not support the process class
class Job;
  process p;
  task run(int d);
    p = process::self();
    #d $display("job done at %0t", $time);
  endtask
endclass

module top;
  process a, b, c, me;
  Job j = new;
  int ticks;
  initial begin
    me = process::self();
    $display("%s", me.status().name());
    fork
      begin a = process::self(); #5 $display("a done at %0t", $time); end
      begin b = process::self(); forever #1 ticks++; end
      begin c = process::self(); fork #100 $display("grandchild not killed"); join_none #100 $display("c not killed"); end
      j.run(3);
    join_none
    #0;
    a.await();
    $display("%s at %0t", a.status().name(), $time);
    b.suspend();
    $display("%s", b.status().name());
    #10 $display("ticks %0d", ticks);
    b.resume();
    #3 $display("ticks %0d", ticks);
    b.kill();
    c.kill();
    $display("%s %s %s", b.status().name(), c.status().name(), j.p.status().name());
    #200 $display("end at %0t", $time);
  end
endmodule

//> RUNNING
//> job done at 3
//> a done at 5
//> FINISHED at 5
//> SUSPENDED
//> ticks 5
//> ticks 8
//> KILLED KILLED FINISHED
//> end at 218
