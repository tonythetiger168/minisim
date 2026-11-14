// IEEE 1800-2023 21.3.3: the format argument of $sformat and $sformatf may be
// a string variable; its specifiers apply to the arguments that follow.
module t;
  string f, s;
  initial begin
    f = "<%0d|%h|%s>";
    $sformat(s, f, -3, 8'h3c, "ok");
    $display("%s", s);
    f = "n=%0d";
    s = $sformatf(f, 42);
    $display("%s", s);
  end
endmodule
//> <-3|3c|ok>
//> n=42
