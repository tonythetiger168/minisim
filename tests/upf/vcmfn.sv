// Function and model VCMs (IEEE 1801-2024 6.30.8, 6.30.9, Annex I.4, I.5):
// run with vcmfn.upf.
//   u_ldo.vout  a real regulator output drives VF through a function VCM
//   u_ana.vr    VF drives a real pin through a function VCM
//   u_ldo2.vout a second regulator drives VM through a model VCM
//   u_ana.vi    VM drives an integer pin through a model VCM
package vcm_pkg;
  import UPF::*;
  function automatic upfSupplyTypeT real2upf(real v);
    upfSupplyTypeT s;
    s.voltage = int'(v * 1.0e6);
    s.state = v >= 0.8 ? FULL_ON : v > 0.2 ? PARTIAL_ON : OFF;
    return s;
  endfunction
  function automatic real upf2real(upfSupplyTypeT s);
    return s.state == OFF ? 0.0 : s.voltage / 1.0e6;
  endfunction
  // not VCM functions: for the error tests
  function automatic upfSupplyTypeT both(upfSupplyTypeT s); return s; endfunction
  function automatic upfSupplyTypeT two(real a, real b); return '0; endfunction
endpackage
// I.4: a model VCM, parameterized
module snap_vcm import UPF::*; #(parameter real on_threshold = 0.2, parameter real snap = 1.0)
    (input real hdl_in, output upfSupplyTypeT upf_out);
  always @(hdl_in)
    if (hdl_in <= on_threshold) upf_out = '{OFF, 0};
    else upf_out = '{FULL_ON, int'(snap * 1.0e6)};
endmodule
module state_vcm import UPF::*; (input upfSupplyTypeT upf_in, output integer hdl_out);
  assign hdl_out = upf_in.state == FULL_ON ? 2 : upf_in.state == PARTIAL_ON ? 1 : 0;
endmodule
module two_in import UPF::*; (input real a, b, output upfSupplyTypeT o);
endmodule
module ldo(output real vout);
  real v = 0.0;
  assign vout = v;
endmodule
module ana(input real vr, input integer vi);
endmodule
module dut;
  real r, r2;
  ldo u_ldo(.vout(r));
  ldo u_ldo2(.vout(r2));
  ana u_ana(.vr(0.0), .vi(0));
  two_in u_two(.a(0.0), .b(0.0), .o());
endmodule
module tb;
  import UPF::*;
  dut u_dut();
  task show(string what);
    upfSupplyTypeT a, b;
    a = get_supply_value("VF");
    b = get_supply_value("VM");
    $display("%0t %s: VF=%s %0d VM=%s %0d | vr=%g vi=%0d", $time, what, a.state.name(), a.voltage,
             b.state.name(), b.voltage, u_dut.u_ana.vr, u_dut.u_ana.vi);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSS", 0.0);
    #1 show("start");
    u_dut.u_ldo.v = 1.1;
    u_dut.u_ldo2.v = 0.9;
    #1 show("on");
    u_dut.u_ldo.v = 0.5;
    u_dut.u_ldo2.v = 0.1;
    #1 show("low");
    $finish;
  end
endmodule
