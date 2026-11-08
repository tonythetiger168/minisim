// IEEE 1800-2023 23.6 Hierarchical names: read and write a signal inside a
// child instance and inside a generate block from the parent.
module child;
  logic [7:0] hidden = 8'h42;
endmodule
module t;
  child u ();
  for (genvar g = 0; g < 2; g++) begin : blk
    logic [3:0] v = g + 1;
  end
  initial begin
    #1 $display("%h %0d %0d", u.hidden, blk[0].v, blk[1].v);
    u.hidden = 8'h99;
    u.hidden[0] = 1'b0;
    blk[1].v = 7;
    $display("%h %0d", u.hidden, blk[1].v);
  end
endmodule
//> 42 1 2
//> 98 7
