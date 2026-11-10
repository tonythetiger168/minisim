// IEEE 1800-2023 20.17: $system runs a shell command; as a function it
// returns the command's exit status (20.17).
//! icarus-skip: Icarus 13 does not support $system
module top;
  int r;
  initial begin
    r = $system("exit 3");
    $display("%0d", r);
    $system("echo from the shell");
    $display("%0d", $system("true"));
  end
endmodule

//> 3
//> from the shell
//> 0
