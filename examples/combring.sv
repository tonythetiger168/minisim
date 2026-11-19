// A ring of 70 always_comb blocks, each writing a default first and its
// value after (IEEE 1800-2023 9.2.2.2). The ring is too large to merge into
// one function, so it stays a cycle of separate blocks: each run changes its
// output and back, and only the final value may wake the next block, or the
// ring wakes itself forever at one time step (msim -compile did, before the
// blocks on a cycle were marked in the compiled schedule).
module combring;
  localparam int N = 70;
  logic en, e;
  logic [N-1:0] x;
  for (genvar k = 0; k < N; k++) begin : g
    always_comb begin
      x[k] = 0;
      if (en) x[k] = (k == 0 ? e : 1'b0) | x[(k + N - 1) % N];
    end
  end
  initial begin
    en = 0; e = 0;
    #1 en = 1; e = 1;
    #1 $display("%h", x);
    #1 e = 0;
    #1 $display("%h", x);
    en = 0;
    #1 $display("%h", x);
    $finish;
  end
endmodule
