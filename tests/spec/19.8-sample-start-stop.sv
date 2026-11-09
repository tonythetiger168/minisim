// IEEE 1800-2023 19.7, 19.8, 19.8.1, 19.11.3: sample() with arguments, stop/start,
// set_inst_name, and type coverage with merge_instances.
//! icarus-skip: Icarus 13 does not support covergroups
module top;
  covergroup cs with function sample(int x);
    option.per_instance = 1;
    type_option.merge_instances = 1;
    cp: coverpoint x { bins b[] = {[0:3]}; }
  endgroup
  cs g1 = new, g2 = new;
  initial begin
    g1.set_inst_name("first");
    g1.sample(0); g2.sample(1);
    g1.stop(); g1.sample(2); g1.start(); g2.sample(1);
    $display("g1 %0.2f g2 %0.2f type %0.2f", g1.get_inst_coverage(), g2.get_inst_coverage(), cs::get_coverage());
    $display("cp %0.2f", g1.cp.get_coverage());
  end
endmodule

//> g1 25.00 g2 25.00 type 50.00
//> cp 50.00
