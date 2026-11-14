// IEEE 1800-2023 21.7.3: the file name of $dumpports and the other
// $dumpports tasks may be a string variable.
module t(input a);
  string fname = "dumpports_var.evcd";
  int fd;
  string line;
  initial begin
    $dumpports(t, fname);
    #10 $dumpportsoff(fname);
    #10 $dumpportson(fname);
    $dumpportsflush(fname);
    fd = $fopen(fname, "r");
    $display("opened %0d", fd != 0);
    if ($fgets(line, fd)) $display("first line read");
  end
endmodule
//> opened 1
//> first line read
//! icarus-skip: Icarus does not write $dumpports files
