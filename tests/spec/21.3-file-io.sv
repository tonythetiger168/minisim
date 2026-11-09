// IEEE 1800-2023 21.3: $fopen, $fdisplay, $fwrite, $fclose, $fgets,
// $fscanf, $feof, $sscanf, $sformatf (21.3).
//! xfail: file I/O is an error
module top;
  int f, r, v, w;
  string s;
  initial begin
    f = $fopen("_regress_io.txt", "w");
    $fdisplay(f, "12 ab");
    $fwrite(f, "line2\n");
    $fclose(f);
    f = $fopen("_regress_io.txt", "r");
    r = $fscanf(f, "%d %h\n", v, w);
    $display("%0d %0d %0d", r, v, w);
    r = $fgets(s, f);
    $display("%0d [%s]", r, s.substr(0, 4));
    r = $fgetc(f);
    $display("%0d %0d", r, $feof(f));
    $fclose(f);
    r = $sscanf("7 x 3.5", "%d %s", v, s);
    $display("%0d %0d %s", r, v, s);
  end
endmodule

//> 2 12 171
//> 6 [line2]
//> -1 1
//> 2 7 x
