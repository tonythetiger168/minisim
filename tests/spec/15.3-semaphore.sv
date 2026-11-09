// IEEE 1800-2023 15.3: A semaphore: new, get, put, try_get (15.3).
//! xfail: semaphores are errors
//! icarus-skip: Icarus 13 does not support semaphores
module top;
  semaphore s;
  initial begin
    s = new(1);
    s.get(1);
    $display("%0d", s.try_get(1));
    fork
      begin s.get(1); $display("got at %0t", $time); end
      #4 s.put(1);
    join
  end
endmodule

//> 0
//> got at 4
