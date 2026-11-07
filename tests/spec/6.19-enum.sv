// IEEE 1800-2017 6.19 Enumerations: a name is its value; values count up from 0.
module t;
  typedef enum logic [1:0] {IDLE, RUN, DONE = 3} st_t;
  st_t s;
  initial begin
    s = RUN;
    $display("%0d %0d %0d", IDLE, s, DONE);
    if (s == RUN) $display("run");
  end
endmodule
//> 0 1 3
//> run
