// IEEE 1800-2023 21.3.7: $ferror returns 0 and leaves an empty message for
// a descriptor with no error, and a nonzero code and its text otherwise.
module t;
  int fd, e;
  string msg;
  initial begin
    fd = $fopen("ferror_test.txt", "w");
    e = $ferror(fd, msg);
    $display("%0d [%s]", e, msg);
    $fclose(fd);
    e = $ferror(32'h8000_0040, msg);
    $display("%0d %0d", e != 0, msg.len() > 0);
  end
endmodule
//> 0 []
//> 1 1
//! icarus-skip: Icarus takes only a 640-bit reg for the $ferror message
