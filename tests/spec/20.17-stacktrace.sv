// IEEE 1800-2023 20.17.2: $stacktrace reports the call stack; the format is
// up to the tool. msim prints the scope of the call.
//! icarus-skip: tool-specific output
module top;
  task automatic f; $stacktrace; endtask
  initial begin f(); $display("after"); end
endmodule
//> $stacktrace: top.f
//> after
