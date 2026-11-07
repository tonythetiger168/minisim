// Never calls $finish: msim -maxtime must stop it. Posedges are at 5, 15, 25, ...;
// -maxtime 25 still runs the slot at time 25, so it prints three ticks.
module spin;
  logic clk = 0;
  always #5 clk = ~clk;
  always @(posedge clk) $display("tick");
endmodule
