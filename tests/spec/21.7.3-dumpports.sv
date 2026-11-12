// IEEE 1800-2023 21.7.3 / 21.7.4: $dumpports writes an extended VCD file
// with the ports of the named instance: inputs as D/U/N/Z, outputs as
// L/H/X/T, an inout by the side that drives it, each with its strength0 and strength1
// digits. $dumpportsoff dumps every port as unknown, $dumpportson and
// $dumpportsall dump the current values, $dumpportsflush writes the file
// out so the test can read it back.
//! icarus-skip: Icarus 13 ignores $dumpports
module dut(input a, input [3:0] bus, output b, inout c);
  assign b = ~a;
  assign c = a ? 1'bz : 1'b0;
endmodule
module top;
  logic a = 0;
  logic [3:0] bus = 4'h5;
  wire b, c;
  dut u(a, bus, b, c);
  int fd;
  string line;
  initial begin
    $dumpports(u, "spec_21_7_3.evcd");
    #1 a = 1;
    #1 bus = 4'bx01z;
    #1 $dumpportsoff("spec_21_7_3.evcd");
    #1 a = 0;
    #1 $dumpportson("spec_21_7_3.evcd");
    #1 $dumpportsall("spec_21_7_3.evcd");
    $dumpportsall("other.evcd");   // not a $dumpports file: ignored (21.7.3.7)
    #1 $dumpportsflush;
    fd = $fopen("spec_21_7_3.evcd", "r");
    void'($fgets(line, fd));   // $version: names the simulator
    while ($fgets(line, fd)) $write("%s", line);
    $fclose(fd);
  end
endmodule
//> $timescale 1ns $end
//> $scope module top.u $end
//> $var port 1 <0 a $end
//> $var port [3:0] <1 bus $end
//> $var port 1 <2 b $end
//> $var port 1 <3 c $end
//> $upscope $end
//> $enddefinitions $end
//> #0
//> $dumpports
//> pD 6 0 <0
//> pDUDU 6060 0606 <1
//> pH 0 6 <2
//> pL 6 0 <3
//> $end
//> #1
//> pU 0 6 <0
//> pL 6 0 <2
//> pF 0 0 <3
//> #2
//> pNDUZ 6600 6060 <1
//> #3
//> $dumpportsoff
//> pN 6 6 <0
//> pNNNN 6666 6666 <1
//> pX 6 6 <2
//> p? 6 6 <3
//> $end
//> #5
//> $dumpportson
//> pD 6 0 <0
//> pNDUZ 6600 6060 <1
//> pH 0 6 <2
//> pL 6 0 <3
//> $end
//> #6
//> $dumpportsall
//> pD 6 0 <0
//> pNDUZ 6600 6060 <1
//> pH 0 6 <2
//> pL 6 0 <3
//> $end
