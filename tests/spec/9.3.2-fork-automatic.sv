// IEEE 1800-2023 9.3.2: An automatic variable declared in a fork block is
// set when the fork runs, once per execution, and its branches share it. A
// join_none fork in a loop therefore gives each set of branches its own
// copy of the loop index. An automatic variable inside a branch is
// initialized when the branch starts, after the loop has moved on.
//! icarus-skip: Icarus 13 does not support automatic variables in a static block
module top;
  initial begin
    for (int i = 0; i < 3; i++)
      fork
        automatic int j = i;
        #1 $display("j %0d", j);
      join_none
    #2;
    for (int i = 0; i < 2; i++)
      fork
        begin automatic int k = i; #1 $display("k %0d", k); end
      join_none
    wait fork;
  end
endmodule

//> j 0
//> j 1
//> j 2
//> k 2
//> k 2
