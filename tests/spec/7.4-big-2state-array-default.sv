// IEEE 1800-2023 6.8 / 7.4: elements of a 2-state unpacked array start at 0.
// A big one (here 2^20 bytes, two dimensions, and a descending range) must
// get its defaults without one setup step per element, which once took
// gigabytes for a 1 MiB memory model.
module top;
  bit [7:0] mem [0:(1 << 20) - 1];
  int       grid [7:0][0:299];
  logic [3:0] fx [0:99];   // 4-state: starts at x
  initial begin
    static int bad = 0;
    for (int i = 0; i < (1 << 20); i += 4093) if (mem[i] !== 8'h00) bad++;
    if (mem[(1 << 20) - 1] !== 0) bad++;
    foreach (grid[a, b]) if (grid[a][b] !== 0) bad++;
    $display("%0d %b", bad, fx[50]);
    mem[12345] = 8'hA5;
    $display("%h %h", mem[12345], mem[12346]);
  end
endmodule

//> 0 xxxx
//> a5 00
