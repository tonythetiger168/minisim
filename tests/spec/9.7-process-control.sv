// IEEE 1800-2023 9.7: The process class: self, status, kill, await (9.7).
//! xfail: the process class is not supported
//! icarus-skip: Icarus 13 does not support the process class
module top;
  process p;
  initial begin
    fork
      begin p = process::self(); #10 $display("not killed"); end
    join_none
    #1 $display("%s", p.status().name());
    p.kill();
    $display("%s", p.status().name());
  end
endmodule

//> WAITING
//> KILLED
