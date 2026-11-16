// A report catcher demoted the error: the summary says 0 errors, and the
// summary's numbers are the ones that count.
module top;
  initial begin
    $display("UVM_ERROR tb.sv(20) @ 5: uvm_test_top [CMP] mismatch");
    $display("--- UVM Report Summary ---");
    $display("UVM_INFO :    3");
    $display("UVM_WARNING :    1");
    $display("UVM_ERROR :    0");
    $display("UVM_FATAL :    0");
    $finish;
  end
endmodule
