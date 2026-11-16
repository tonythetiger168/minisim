// 25.9, 9.4.2: an event control in a class may name a bit or part select of a
// virtual interface member or of a class member, mixed with whole members:
// the wait wakes on that bit's edges only.
//! icarus-skip: Icarus 13 does not support virtual interfaces
interface ifc; logic [2:0] state; logic in; endinterface
class mon;
  virtual ifc vif;
  logic [3:0] r;
  task run();
    repeat (3) begin
      @(posedge vif.in or negedge vif.in or posedge vif.state[0] or negedge vif.state[0]);
      $display("%0t vif in=%b s=%b", $time, vif.in, vif.state);
    end
  endtask
  task run2();
    repeat (2) begin
      @(vif.state[2:1] or posedge r[3]);
      $display("%0t sel s=%b r=%b", $time, vif.state, r);
    end
  endtask
endclass
module tb;
  ifc i(); mon m;
  initial begin
    m = new; m.vif = i; m.r = 0;
    i.in = 0; i.state = 0;
    fork m.run(); m.run2(); join_none
    #1 i.in = 1;
    #1 i.state = 3'b001;
    #1 i.state = 3'b011;   // bit 0 unchanged: only run2 wakes
    #1 m.r = 4'b0111;      // no posedge on r[3]
    #1 m.r = 4'b1000;
    #1 i.state = 3'b010;
    #1 $finish;
  end
endmodule
//> 1 vif in=1 s=000
//> 2 vif in=1 s=001
//> 3 sel s=011 r=0000
//> 5 sel s=011 r=1000
//> 6 vif in=1 s=010
