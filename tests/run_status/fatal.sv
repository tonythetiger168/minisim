module top;
  initial begin
    #3 $fatal(1, "stop here");
  end
endmodule
