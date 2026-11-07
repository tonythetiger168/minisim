// IEEE 1800-2023 4.9.4 Nonblocking assignment updates happen after the active
// region; 21.2.2 $strobe prints in the postponed region, after NBA.
module t;
  logic b;
  initial begin
    b = 0;
    b <= 1;
    $display("display %b", b);
    $strobe("strobe %b", b);
    #1 $display("later %b", b);
  end
endmodule
//> display 0
//> strobe 1
//> later 1
