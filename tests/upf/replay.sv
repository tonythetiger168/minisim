// sim_replay_control (IEEE 1801-2024 6.60) and save_upf (6.42): run with
// replay.upf. The ROM model loads its contents in an initial block; when
// its domain powers up again, the block runs again and the contents come
// back (also at the first power-up, after time 0). The PLL model's
// initial block is not replayed.
module rom(input logic clk, input logic we, input logic [1:0] a, output logic [7:0] q);
  logic [7:0] mem [4];
  always_ff @(posedge clk) if (we) mem[a] <= 8'hee;   // a write port: mem is state, lost at power-down
  initial begin
    mem[0] = 8'h11; mem[1] = 8'h22; mem[2] = 8'h33; mem[3] = 8'h44;
  end
  assign q = mem[a];
endmodule
module pll(output logic lock);
  initial lock = 1;
endmodule
module dut(input logic [1:0] a, output logic [7:0] q, output logic lock);
  rom u_rom(.clk(1'b0), .we(1'b0), .a, .q);
  pll u_pll(.lock);
endmodule
module tb;
  import UPF::*;
  logic [1:0] a = 2;
  logic [7:0] q;
  logic lock;
  dut u_dut(.*);
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSS", 0.0);
    #1 $display("%0t VSW not on yet: q=%h", $time, q);
    supply_on("VSW", 1.0);
    #1 $display("%0t first power-up, the ROM is loaded: q=%h lock=%b", $time, q, lock);
    supply_off("VSW");
    #1 $display("%0t off: q=%h lock=%b", $time, q, lock);
    supply_on("VSW", 1.0);
    #1 $display("%0t on again, the ROM is loaded again: q=%h lock=%b", $time, q, lock);
    a = 1;
    #1 $display("%0t q=%h", $time, q);
    $finish;
  end
endmodule
