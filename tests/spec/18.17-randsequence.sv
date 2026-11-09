// IEEE 1800-2023 18.17: randsequence runs productions in order (18.17).
//! icarus-skip: Icarus 13 does not support randsequence
module top;
  initial randsequence (main)
    main : first second;
    first : { $display("first"); };
    second : { $display("second"); };
  endsequence
endmodule

//> first
//> second
