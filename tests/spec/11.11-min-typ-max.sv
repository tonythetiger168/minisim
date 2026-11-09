// IEEE 1800-2023 11.11: A min:typ:max delay uses the typical value by
// default (11.11).
module top;
  logic a;
  initial begin
    #(1:2:3) a = 1;
    $display("%0t", $time);
  end
endmodule

//> 2
