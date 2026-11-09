// IEEE 1800-2023 21.5: $writememh / $writememb write a memory that
// $readmemh / $readmemb read back (21.5).
module top;
  logic [7:0] m[4], n[4];
  initial begin
    foreach (m[i]) m[i] = 8'h10 * i + 1;
    $writememh("_regress_mem.hex", m);
    $readmemh("_regress_mem.hex", n);
    $display("%h %h %h %h", n[0], n[1], n[2], n[3]);
    foreach (m[i]) m[i] = 8'h0f - i;
    $writememh("_regress_mem.hex", m);
    $readmemh("_regress_mem.hex", n);
    $display("%h %h %h %h", n[0], n[1], n[2], n[3]);
    $writememb("_regress_mem.bin", m, 1, 2);
    n[1] = 0; n[2] = 0;
    $readmemb("_regress_mem.bin", n, 1, 2);
    $display("%h %h", n[1], n[2]);
  end
endmodule

//> 01 11 21 31
//> 0f 0e 0d 0c
//> 0e 0d
