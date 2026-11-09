// IEEE 1800-2023 26.7: The std package holds process, semaphore and mailbox
// (26.7).
//! xfail: the std package classes are not supported
//! icarus-skip: Icarus 13 does not support std::semaphore
module top;
  std::semaphore s = new(1);
  initial $display("%0d", s.try_get());
endmodule

//> 1
