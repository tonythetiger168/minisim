// IEEE 1800-2023 18.14.2, 18.13.3: every process has its own random
// generator, seeded from its parent's when forked. Values one process
// draws do not depend on how many another process draws; srandom,
// get_randstate and set_randstate on a process object act on it.
//! icarus-skip: Icarus 13 does not support std::process
module top;
  int a1[2], a2[2], s1, s2;
  string st;
  int x, y, z;
  initial begin
    static process p = process::self();
    for (int run = 0; run < 2; run++) begin
      p.srandom(5);
      fork
        begin a1[run] = $urandom; a2[run] = $urandom; end
        begin repeat (run * 4 + 1) void'($urandom); end
      join
    end
    $display("%0d", a1[0] == a1[1] && a2[0] == a2[1] && a1[0] != a2[0]);
    st = p.get_randstate();
    x = $urandom;
    p.set_randstate(st);
    y = $urandom;
    $display("%0d", x == y);
    p.srandom(9); x = $urandom;
    p.srandom(9); void'($urandom_range(3)); p.srandom(9); y = $urandom;
    $display("%0d", x == y);
  end
  // another top-level process drawing does not move this one's values
  initial repeat (3) void'($urandom);
endmodule

//> 1
//> 1
//> 1
