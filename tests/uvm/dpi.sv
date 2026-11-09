// UVM's C code (IEEE 1800.2-2020 19.6 HDL access, F.5 / Annex C regular
// expressions, G.1 command line processor), built by msim -uvm from
// uvm/uvm_dpi_minisim.cc: uvm_re_match, uvm_cmdline_processor and
// +uvm_set_config_int, uvm_hdl_check_path / read / deposit / force /
// release on variables and nets, and a register model read and written
// through its backdoor.
//! args: +mykey=a +mykey=b +uvm_set_config_int=uvm_test_top,knob,7 +UVM_NO_RELNOTES
`include "uvm_macros.svh"
module dut(input logic clk);
  logic [7:0] r = 8'h5a;
  logic [99:0] wide = {4'h9, 96'h0123456789abcdef01234567};
  wire [7:0] w = r + 1;
  logic [15:0] ctrl = 16'h00c3;
  always @(posedge clk) r <= r;
endmodule
module top;
  import uvm_pkg::*;
  logic clk = 0;
  always #5 clk = ~clk;
  dut u(clk);
  class ctrl_reg extends uvm_reg;
    `uvm_object_utils(ctrl_reg)
    rand uvm_reg_field f;
    function new(string name = "ctrl"); super.new(name, 16, UVM_NO_COVERAGE); endfunction
    virtual function void build();
      f = uvm_reg_field::type_id::create("f");
      f.configure(this, 16, 0, "RW", 0, 0, 1, 1, 0);
    endfunction
  endclass
  class blk extends uvm_reg_block;
    `uvm_object_utils(blk)
    rand ctrl_reg ctrl;
    function new(string name = "blk"); super.new(name, UVM_NO_COVERAGE); endfunction
    virtual function void build();
      default_map = create_map("map", 0, 2, UVM_LITTLE_ENDIAN);
      ctrl = ctrl_reg::type_id::create("ctrl");
      ctrl.configure(this, null, "ctrl");
      ctrl.build();
      default_map.add_reg(ctrl, 0, "RW");
      add_hdl_path("top.u");
      lock_model();
    endfunction
  endclass
  class t extends uvm_test;
    `uvm_component_utils(t)
    uvm_bitstream_t knob;
    blk rm;
    function new(string n, uvm_component p); super.new(n, p); endfunction
    function void build_phase(uvm_phase phase);
      rm = blk::type_id::create("rm");
      rm.build();
    endfunction
    task run_phase(uvm_phase phase);
      uvm_cmdline_processor clp = uvm_cmdline_processor::get_inst();
      string vals[$];
      uvm_hdl_data_t v;
      uvm_status_e st;
      uvm_reg_data_t d;
      phase.raise_objection(this);
      // regular expressions: 0 is a match
      $display("re %0d %0d %0d", uvm_re_match("^ab+c$", "abbbc"), uvm_re_match("^ab+c$", "ac"),
               uvm_re_match(uvm_glob_to_re("top.*.drv"), "top.env.drv"));
      // command line
      $display("plusargs %0d %s %s", clp.get_arg_values("+mykey=", vals), vals[0], vals[1]);
      $display("tool %s", clp.get_tool_name());
      void'(uvm_config_db#(uvm_bitstream_t)::get(this, "", "knob", knob));
      $display("knob %0d", knob);
      // HDL access
      $display("check %0d %0d", uvm_hdl_check_path("top.u.r"), uvm_hdl_check_path("top.u.nope"));
      void'(uvm_hdl_read("top.u.r", v)); $display("read %h", v[7:0]);
      void'(uvm_hdl_read("top.u.wide", v)); $display("wide %h", v[99:0]);
      void'(uvm_hdl_deposit("top.u.r", 8'h11)); #1 $display("deposit %h %h", u.r, u.w);
      void'(uvm_hdl_force("top.u.r", 8'h22)); #1 $display("force %h %h", u.r, u.w);
      void'(uvm_hdl_force("top.u.w", 8'hee)); #1 $display("force net %h", u.w);
      void'(uvm_hdl_release("top.u.w")); #1 $display("release net %h", u.w);
      void'(uvm_hdl_release_and_read("top.u.r", v)); $display("release %h", v[7:0]);
      // register model backdoor
      rm.ctrl.read(st, d, UVM_BACKDOOR);
      $display("reg read %s %h", st.name(), d[15:0]);
      rm.ctrl.write(st, 16'hbeef, UVM_BACKDOOR);
      #1 $display("reg write %s %h", st.name(), u.ctrl);
      phase.drop_objection(this);
    endtask
  endclass
  initial run_test("t");
endmodule

//> Applying config setting from the command line: +uvm_set_config_int=uvm_test_top,knob,7
//> re 0 1 0
//> plusargs 2 a b
//> tool minisim
//> knob 7
//> check 1 0
//> read 5a
//> wide 90123456789abcdef01234567
//> deposit 11 12
//> force 22 23
//> force net ee
//> release net 23
//> release 22
//> reg read UVM_IS_OK 00c3
//> reg write UVM_IS_OK beef
//> UVM_ERROR :    0
//> UVM_FATAL :    0
