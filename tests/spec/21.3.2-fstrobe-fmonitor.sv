// IEEE 1800-2023 21.3.2: $fstrobe and $fmonitor write to a file like
// $strobe and $monitor; several $fmonitors can be active at once, and
// $fclose cancels the ones on the closed descriptor.
module top;
  integer f1, f2, r;
  logic [7:0] a = 0, b = 0;
  string s;
  initial begin
    f1 = $fopen("_spec_fmon1.txt", "w");
    f2 = $fopen("_spec_fmon2.txt", "w");
    $fmonitor(f1, "f1 t=%0t a=%0d", $time, a);
    $fmonitor(f2, "f2 t=%0t b=%0d", $time, b);
    $fstrobeh(f1, "strobe a=", a);
    a = 1;
    #1 a = 2; b = 5;
    #1 a = 3; b = 6;
    #1 $fclose(f2);
    b = 7;
    #1 a = 4;
    $fclose(f1);
    #1 a = 5;
    f1 = $fopen("_spec_fmon1.txt", "r");
    while ($fgets(s, f1)) $write("%s", s);
    $fclose(f1);
    f2 = $fopen("_spec_fmon2.txt", "r");
    while ($fgets(s, f2)) $write("%s", s);
    $fclose(f2);
  end
endmodule
//> f1 t=0 a=1
//> strobe a=01
//> f1 t=1 a=2
//> f1 t=2 a=3
//> f2 t=0 b=0
//> f2 t=1 b=5
//> f2 t=2 b=6
