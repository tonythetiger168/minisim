// IEEE 1800-2023 21.3.4.4: $fread reads binary data, big-endian, (w+7)/8
// bytes per value: into a variable, or into a memory from start for at
// most count elements. The result is the number of bytes read.
module top;
  integer fd, code, i;
  logic [15:0] w;
  logic [8:0] nine;
  logic [7:0] mem [0:7];
  logic [15:0] mw [3:0];
  initial begin
    fd = $fopen("_spec_fread.bin", "wb");
    for (i = 0; i < 13; i++) $fwrite(fd, "%c", 8'h41 + i);
    $fclose(fd);
    fd = $fopen("_spec_fread.bin", "rb");
    code = $fread(w, fd);
    $display("w=%h code=%0d", w, code);
    code = $fread(nine, fd);   // 2 bytes, the low 9 bits kept
    $display("nine=%h code=%0d", nine, code);
    foreach (mem[k]) mem[k] = 0;
    code = $fread(mem, fd, 2, 3);
    $display("mem=%h %h %h %h %h code=%0d", mem[1], mem[2], mem[3], mem[4], mem[5], code);
    code = $fread(mw, fd, , 2);
    $display("mw=%h %h %h code=%0d", mw[0], mw[1], mw[2], code);
    code = $fread(w, fd);
    $display("w=%h code=%0d", w, code);
    code = $fread(w, fd);
    $display("at end code=%0d w=%h", code, w);
    $fclose(fd);
  end
endmodule
//> w=4142 code=2
//> nine=144 code=2
//> mem=00 45 46 47 00 code=3
//> mw=4849 4a4b xxxx code=4
//> w=4c4d code=2
//> at end code=0 w=4c4d
