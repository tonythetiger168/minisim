// IEEE 1800-2023 19.4: covergroups embedded in classes. Each object gets
// its own instance, created in the constructor; coverpoints, guards and
// options read that object's properties whoever samples it: sample from a
// method, sample from outside through the handle, a sampling event on a
// property, on a member of another object, and on a clock property; a
// constructor argument and a sample function argument; local properties;
// a derived class; and a shallow copy, whose covergroup handle is null 8.12.
//! icarus-skip: Icarus 13 does not support covergroups
module top;
  class Helper;
    int m_ev;
  endclass
  class xyz;
    bit [3:0] m_x;
    int m_y;
    bit m_z;
    covergroup cov1 @m_z;
      coverpoint m_x { bins b[] = {[0:3]}; }
      coverpoint m_y { bins one = {1}; bins two = {2}; }
    endgroup
    function new(); cov1 = new; endfunction
  endclass
  class MyClass;
    Helper m_obj;
    int m_a;
    covergroup Cov @(m_obj.m_ev);
      coverpoint m_a { bins b[] = {[1:4]}; }
    endgroup
    function new(); m_obj = new; Cov = new; endfunction
  endclass
  class MC;
    logic [3:0] m_x;
    local logic m_z;
    bit clk;
    bit m_e;
    covergroup cv1 @(posedge clk); coverpoint m_x { bins b[] = {[0:7]}; } endgroup
    covergroup cv2 @m_e; coverpoint m_z; endgroup
    function new(); cv1 = new; cv2 = new; endfunction
    function void setz(logic z); m_z = z; endfunction
  endclass
  class Packet;
    bit [3:0] kind;
    int len;
    bit [1:0] mode;
    covergroup cg;
      cp_kind: coverpoint kind { bins lo = {[0:7]}; bins hi = {[8:15]}; }
      cp_len: coverpoint len { bins sml = {[0:63]}; bins bg = {[64:$]}; }
    endgroup
    covergroup cg2 (int lim) with function sample(int s);
      option.at_least = 1;
      coverpoint s { bins lo = {[0:lim-1]}; bins hi = {[lim:$]}; }
      coverpoint len iff (mode == 3) { bins any = {[0:$]}; }
    endgroup
    function new(int lim = 10);
      cg = new;
      cg2 = new(lim);
    endfunction
    function void put(bit [3:0] k, int l);
      kind = k; len = l;
      cg.sample();
    endfunction
  endclass
  class BigPacket extends Packet;
    function new(); super.new(100); endfunction
  endclass

  xyz o1, o2;
  MyClass mc;
  MC m;
  Packet p1, p2, p3;
  BigPacket bp;
  initial begin
    o1 = new; o2 = new; mc = new; m = new;
    #1 o1.m_x = 1; o1.m_y = 1; o1.m_z = 1;
    #1 o1.m_x = 2; o1.m_z = 0;
    #1 o2.m_x = 3; o2.m_y = 2; o2.m_z = 1;
    #1 mc.m_a = 2; mc.m_obj.m_ev = 5;
    #1 mc.m_a = 3; mc.m_obj.m_ev = 6;
    #1 m.m_x = 4; m.clk = 1;
    #1 m.clk = 0; m.m_x = 5;
    #1 m.setz(1); m.m_e = 1;
    #1 $display("o1 %0.2f o2 %0.2f", o1.cov1.get_inst_coverage(), o2.cov1.get_inst_coverage());
    $display("mc %0.2f", mc.Cov.get_inst_coverage());
    $display("cv1 %0.2f cv2 %0.2f", m.cv1.get_inst_coverage(), m.cv2.get_inst_coverage());

    p1 = new; p2 = new; bp = new;
    p1.put(3, 10);
    p2.put(12, 100);
    p2.put(4, 20);
    p1.kind = 9; p1.cg.sample();
    $display("p1 %0.2f p2 %0.2f", p1.cg.get_inst_coverage(), p2.cg.get_inst_coverage());
    p1.mode = 1; bp.mode = 3; bp.len = 7;
    p1.cg2.sample(50); bp.cg2.sample(50);
    $display("p1.cg2 %0.2f bp.cg2 %0.2f", p1.cg2.get_inst_coverage(), bp.cg2.get_inst_coverage());
    p3 = new p1;
    $display("copy %0d", p3.cg == null);
  end
endmodule
//> o1 50.00 o2 37.50
//> mc 50.00
//> cv1 12.50 cv2 50.00
//> p1 75.00 p2 100.00
//> p1.cg2 25.00 bp.cg2 75.00
//> copy 1
