// IEEE 1800-2023 21.3.4.4: $fread(mem, fd, , count) leaves out start: the
// load begins at the lowest address and stops after count elements.
//! icarus-skip: Icarus 13 rejects an empty argument to $fread
module top;
  integer fd, code, i;
  logic [7:0] m [4:7];
  initial begin
    fd = $fopen("_spec_fread_count.bin", "wb");
    for (i = 0; i < 4; i++) $fwrite(fd, "%c", 8'h61 + i);
    $fclose(fd);
    fd = $fopen("_spec_fread_count.bin", "rb");
    foreach (m[k]) m[k] = 0;
    code = $fread(m, fd, , 2);
    $display("%h %h %h %h code=%0d", m[4], m[5], m[6], m[7], code);
    $fclose(fd);
  end
endmodule
//> 61 62 00 00 code=2
