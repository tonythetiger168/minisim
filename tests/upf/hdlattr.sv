// UPF attributes in the HDL (IEEE 1801-2024 5.6, 5.7): run with
// hdlattr.upf. The HDL says the macro is a hard macro whose outputs follow
// its supply, and gives the clamp values of the core's outputs; the UPF
// overrides one of them.
(* UPF_is_hard_macro = "TRUE", UPF_simstate_behavior = "PORT_CORR_ONLY" *)
module macro(input logic [3:0] d, output logic [3:0] q);
  logic [3:0] keep;
  assign keep = ~d;
  assign q = keep;
endmodule
module core(input logic [3:0] d,
            (* UPF_clamp_value = "1" *) output logic [3:0] y,
            (* UPF_clamp_value = "1" *) output logic [3:0] z);
  assign y = d;
  assign z = d;
endmodule
module dut(input logic iso, input logic [3:0] d, output logic [3:0] qm, y, z);
  macro u_m(.d, .q(qm));
  core u_c(.d, .y, .z);
endmodule
module tb;
  import UPF::*;
  logic iso = 0;
  logic [3:0] d = 4'h5, qm, y, z;
  dut u_dut(.*);
  task show(string what);
    $display("%0t %s: qm=%h keep=%h y=%h z=%h", $time, what, qm, u_dut.u_m.keep, y, z);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSW", 1.0);
    supply_on("VSS", 0.0);
    #1 show("on");
    iso = 1;
    supply_off("VSW");
    #1 show("off: y clamps to 1 from the HDL, z to 0 from the UPF");
    supply_on("VSW", 1.0);
    iso = 0;
    #1 show("on again");
    $finish;
  end
endmodule
