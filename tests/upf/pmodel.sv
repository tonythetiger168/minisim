// Power models (IEEE 1801-2024 6.3, 6.9, 6.11, 6.31, 6.32): run with
// pmodel.upf. Two instances of the macro `mac` get the power model MAC_PM,
// one by name and one through "every instance of the model"; the macro
// `rom` gets a legacy begin_power_model model. Each model has its own
// switched core domain, isolated on the way out.
module mac_core(input logic [3:0] a, output logic [3:0] y);
  assign y = a + 4'd1;
endmodule
module mac(input logic [3:0] a, input logic iso, output logic [3:0] y);
  mac_core u_core(.a, .y);
endmodule
module rom(input logic [3:0] a, output logic [3:0] y);
  assign y = ~a;
endmodule
module dut(input logic [3:0] a, input logic iso0, input logic iso1, output logic [3:0] y0, y1, y2);
  mac u_mac0(.a, .iso(iso0), .y(y0));
  mac u_mac1(.a, .iso(iso1), .y(y1));
  rom u_rom(.a, .y(y2));
endmodule
module tb;
  import UPF::*;
  logic [3:0] a = 4'h3, y0, y1, y2;
  logic iso0 = 0, iso1 = 0;
  dut u_dut(.*);
  task show(string what);
    $display("%0t %s: y0=%h y1=%h y2=%h", $time, what, y0, y1, y2);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSW0", 1.0);
    supply_on("VSW1", 1.0);
    supply_on("VROM", 1.0);
    supply_on("VSS", 0.0);
    #1 show("on");
    iso0 = 1;
    supply_off("VSW0");
    #1 show("mac0 off, isolated to its clamp 0");
    iso1 = 1;
    supply_off("VSW1");
    #1 show("mac1 off, clamp 1 from -parameters");
    supply_off("VROM");
    #1 show("rom off");
    supply_on("VSW0", 1.0);
    supply_on("VSW1", 1.0);
    supply_on("VROM", 1.0);
    iso0 = 0;
    iso1 = 0;
    #1 show("on again");
    $finish;
  end
endmodule
