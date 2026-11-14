// IEEE 1800-2023 21.2.3: $monitoroff stops $monitor printing, $monitoron
// starts it again and prints the monitored values at once.
module t;
  int a;
  initial begin
    $monitor("a=%0d", a);
    #1 a = 1;
    #1 $monitoroff; a = 2;
    #1 a = 3;
    #1 $monitoron;
    #1 a = 4;
  end
endmodule
//> a=0
//> a=1
//> a=3
//> a=4
