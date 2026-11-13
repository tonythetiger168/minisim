// 6.12 / 21.7: real and shortreal variables are dumped as $var real, and
// their values as r<number> lines. The test reads its own VCD back.
module tb;
//! icarus-skip: VCD identifier codes and order are tool-specific
  real gain = 0.5;
  shortreal level = -4.0;
  logic [1:0] mode = 2'b10;
  initial begin
    int fd, n;
    string line;
    $dumpfile("spec_6_12_real.vcd");
    $dumpvars(0, tb);
    #1 gain = 3.75; level = 1.25;
    #1 $dumpflush;
    fd = $fopen("spec_6_12_real.vcd", "r");
    while (!$feof(fd)) begin
      n = $fgets(line, fd);
      if (n > 0 && (line.substr(0, 8) == "$var real" || line[0] == "r"))
        $write("%s", line);
    end
    $fclose(fd);
    $finish;
  end
endmodule
//> $var real 64 ! gain $end
//> $var real 64 " level $end
//> r0.5 !
//> r-4 "
//> r3.75 !
//> r1.25 "
