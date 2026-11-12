// Supply ports connected by their pg_type (IEEE 1801-2024 4.5.4.6.2, 6.14,
// 6.15, 6.51): run with pgconn.upf.
//   u_m1  in PD_A: VDD and VSS get PD_A's primary supply automatically
//   u_m2  in PD_A, but connect_supply_set -connect gives VDD the set SS_B
//   every macro's VBK (pg_type backup_power) gets VBAT through
//         connect_supply_net -pg_type -cells
module macro((* UPF_pg_type = "primary_power" *) input logic VDD,
             input logic VSS, VBK, output logic ok, bk);
  // the default conversion (9.1) gives 1 for FULL_ON, ground pins too
  assign ok = VDD === 1'b1 && VSS === 1'b1;
  assign bk = VBK;
endmodule
module dut;
  logic ok1, ok2, bk1, bk2;
  macro u_m1(.VDD(1'b0), .VSS(1'b1), .VBK(1'b0), .ok(ok1), .bk(bk1));
  macro u_m2(.VDD(1'b0), .VSS(1'b1), .VBK(1'b0), .ok(ok2), .bk(bk2));
endmodule
module tb;
  import UPF::*;
  dut u_dut();
  task show(string what);
    $display("%0t %s: m1 VDD=%b ok=%b bk=%b | m2 VDD=%b ok=%b bk=%b", $time, what, u_dut.u_m1.VDD, u_dut.ok1, u_dut.bk1,
             u_dut.u_m2.VDD, u_dut.ok2, u_dut.bk2);
  endtask
  initial begin
    #1 show("all off");
    supply_on("VDD", 1.0);
    supply_on("VSS", 0.0);
    #1 show("VDD on");
    supply_on("VDDB", 0.9);
    #1 show("VDDB on");
    supply_on("VBAT", 3.0);
    #1 show("VBAT on");
    supply_off("VDD");
    #1 show("VDD off");
    $finish;
  end
endmodule
