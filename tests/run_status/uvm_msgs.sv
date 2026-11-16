// Lines shaped like UVM's reports, as a UVM run prints them.
module top;
  initial begin
    $display("UVM_INFO tb.sv(10) @ 0: reporter [START] starting");
    $display("UVM_ERROR tb.sv(20) @ 5: uvm_test_top [CMP] mismatch");
    $display("not UVM_ERROR at the start of a line");
    $display("UVM_ERRORS is not a severity");
    $finish;
  end
endmodule
