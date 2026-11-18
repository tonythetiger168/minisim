// Processes msim -compile used to leave to the interpreter (mixed mode, no
// cycle domain), as in OpenTitan: a named initial block nothing disables, a
// clocked block with a $display, and an always_comb that calls an automatic
// task writing bits of its output argument at run-time indices.
module nomix;
  logic clk = 0, rst = 1;
  logic [7:0] cnt;
  logic [15:0] unit;
  logic [8:0] nq, pq;
  logic [7:0] perm_test;
  localparam logic [2:0] Perm [8] = '{3'd2, 3'd0, 3'd1, 3'd3, 3'd7, 3'd5, 3'd4, 3'd6};

  task automatic fmt(output logic [8:0] n, output logic [8:0] p, input logic [15:0] d);
    n[8] = 1'b1;
    p[8] = d[0];
    for (int unsigned b = 0; b < 8; b++) begin
      n[b] = d[2*b+1];
      p[b] = d[2*b];
    end
  endtask

  initial begin : p_perm_check
    perm_test = '0;
    for (int k = 0; k < 8; k++) perm_test[Perm[k]] = 1'b1;
    if (!(&perm_test)) $display("bad permutation");
  end

  always #5 clk = ~clk;
  initial begin
    #12 rst = 0;
    #200 $display("done cnt=%0d nq=%h pq=%h perm=%h", cnt, nq, pq, perm_test);
    $finish;
  end

  always_ff @(posedge clk or posedge rst)
    if (rst) cnt <= '0;
    else cnt <= cnt + 8'd3;

  assign unit = {cnt, ~cnt} ^ 16'h5a3c;

  always_comb fmt(nq, pq, unit);

  always_ff @(negedge clk)
    if (cnt[1:0] == 2'd2) $display("%0t: cnt=%0d nq=%h pq=%h", $time, cnt, nq, pq);
endmodule
