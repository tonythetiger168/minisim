// A system-level model in C (a SystemC or TLM model, IEEE 1801-2024 5.14)
// follows the power intent through include/msim_upf.h: run with
// syslevel.upf and syslevel.c. The model counts energy only while its
// domain's supply is ON, and reads the domain's simstate.
module m;
  logic busy = 0;   // the RTL side of the model
endmodule
module tb;
  import UPF::*;
  import "DPI-C" function int sys_step(output int sim);
  m u_m();
  int e, sim;
  task step(string what);
    e = sys_step(sim);
    $display("%0t %s: energy=%0d simstate=%0d", $time, what, e, sim);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSS", 0.0);
    supply_on("VDD_SYS", 1.0);
    #1 step("on");
    #1 step("on");
    supply_off("VDD_SYS");
    #1 step("off");
    supply_on("VDD_SYS", 1.0);
    #1 step("on again");
    $finish;
  end
endmodule
