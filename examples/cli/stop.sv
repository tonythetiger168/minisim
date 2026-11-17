// $stop ends a batch run and says where on standard error (20.2); $stop(0)
// prints nothing.
module stop_demo;
  initial begin
    $display("before");
    #5 $stop;
    $display("not reached");
  end
endmodule
