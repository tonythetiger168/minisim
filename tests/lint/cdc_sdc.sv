// Crossings judged by an SDC file (mlint -sdc): which clocks are related is
// the file's to say.
module cdc_sdc_div(input logic clk, rst_n, output logic q);
  always_ff @(posedge clk or negedge rst_n) if (!rst_n) q <= 0; else q <= ~q;
endmodule

module cdc_sdc(input logic aclk, bclk, cclk, tclk, test_mode, rst_n, d,
               output logic qa, qb, qc, qd, qm, qu);
  // A divided clock, and a path to and from its master: synchronous once the
  // file has a create_generated_clock for it.
  logic div2, a0, d0;
  cdc_sdc_div u_div(.clk(aclk), .rst_n(rst_n), .q(div2));
  always_ff @(posedge aclk) a0 <= d;
  always_ff @(posedge div2) d0 <= a0;
  always_ff @(posedge aclk) qa <= d0;

  // aclk -> bclk with no synchronizer.
  always_ff @(posedge bclk) qb <= a0;

  // The same crossing from the divided clock: it goes with its master.
  always_ff @(posedge bclk) qd <= d0;

  // bclk -> cclk.
  logic b0;
  always_ff @(posedge bclk) b0 <= d;
  always_ff @(posedge cclk) qc <= b0;

  // A test clock mux: cclk or tclk, never both.
  logic mclk, c0;
  assign mclk = test_mode ? tclk : cclk;
  always_ff @(posedge cclk) c0 <= d;
  always_ff @(posedge mclk) qm <= c0;

  // A clock the file does not define.
  logic uclk;
  always_ff @(posedge aclk) uclk <= ~uclk;
  always_ff @(posedge uclk) qu <= d;
endmodule
