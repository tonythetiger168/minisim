// IEEE 1800-2023 6.8, 21.6: a static variable's initializer may call a
// system function that writes an argument, such as $value$plusargs or
// $sscanf; it runs before any initial or always procedure starts.
module top;
  string mode;
  int n, got;
  bit has_mode = $value$plusargs("MODE=%s", mode);
  bit has_none = |($value$plusargs("NOSUCH=%d", n));
  int fields = $sscanf("12 34", "%d %d", n, got);
  initial $display("%0d %s %0d %0d %0d %0d", has_mode, mode, has_none, fields, n, got);
endmodule
//! args: +MODE=fast
//> 1 fast 0 2 12 34
