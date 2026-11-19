// IEEE 1800-2023 19.7, 19.7.1, 19.11.3: the option and type_option members
// of a covergroup, coverpoint or cross can be read and written by the
// program; a type_option write applies to every instance, and changing it
// changes how type coverage is computed afterwards.
//! icarus-skip: Icarus does not support covergroups
module top;
  covergroup g with function sample(bit a, bit b);
    option.per_instance = 1;
    type_option.merge_instances = 1;
    pa: coverpoint a { type_option.weight = 0; }
    pb: coverpoint b;
  endgroup
  covergroup h;
    q: coverpoint 1'b0;
  endgroup
  g x = new, y = new;
  h z;
  initial begin
    $display("%0d %0d %0d", x.option.per_instance, x.option.weight, x.option.goal);
    $display("%0d %0d", g::type_option.merge_instances, y.type_option.merge_instances);
    $display("%0d %0d %0d", x.pa.type_option.weight, x.pb.type_option.weight, x.pb.option.at_least);
    x.sample(0, 0);
    y.sample(0, 1);
    $display("%0.2f", x.get_coverage());
    g::type_option.merge_instances = 0;
    $display("%0d %0.2f", x.type_option.merge_instances, x.get_coverage());
    x.sample(1, 0);
    $display("%0.2f %0.2f", x.get_inst_coverage(), x.get_coverage());
    y.option.weight = 3;
    $display("%0d %0.2f", y.option.weight, x.get_coverage());
    x.pa.option.weight = 0;
    $display("%0d %0.2f %0.2f", x.pa.option.weight, x.get_inst_coverage(), x.get_coverage());
    $display("%0d", h::type_option.goal);
    h::type_option.goal = 90;
    z = new;
    $display("%0d %0d", z.type_option.goal, h::type_option.goal);
  end
endmodule
//> 1 1 100
//> 1 1
//> 0 1 1
//> 100.00
//> 0 50.00
//> 75.00 62.50
//> 3 56.25
//> 0 50.00 50.00
//> 100
//> 90 90
