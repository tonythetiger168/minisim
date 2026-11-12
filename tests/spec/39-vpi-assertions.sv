// IEEE 1800-2023 39.3, 39.4, 39.5: the assertion API. The assertions of
// the design are objects (vpiAssert, vpiCover, vpiImmediateAssert, ...)
// found by iteration from NULL or from their scope. Callbacks on one
// assertion report each attempt's start, success (vacuous or not),
// failure and disabled evaluation with the attempt's start time; control
// through vpi_control (disable, enable, reset, pass action off) and through
// $assertoff reaches the callbacks of the assertion and of the system.
//! icarus-skip: Icarus 13 has no assertion API
//! args: data/vpi/assertions.c
module top;
  logic clk = 0, req = 0, ack = 0, rst = 0;
  always #5 clk = ~clk;
  a_req: assert property (@(posedge clk) disable iff (rst) req |-> ##1 ack);
  c_ack: cover property (@(posedge clk) ack);
  initial begin
    $watch;
    @(negedge clk) req = 1;            // 10: attempt at 15 needs ack at 25
    @(negedge clk) begin req = 0; ack = 1; end
    @(negedge clk) begin req = 1; ack = 0; end   // 30: the attempt at 35 fails at 45
    @(negedge clk) req = 0;
    @(negedge clk) begin req = 1; rst = 0; end   // 50
    #2 rst = 1;                                   // 52: the attempt at 55 is disabled
    @(negedge clk) begin rst = 0; req = 0; end
    $control(0);                                  // disable a_req
    @(negedge clk) req = 1;
    @(negedge clk) req = 0;
    $control(1);                                  // enable it, reset it
    $assertoff(0, top.c_ack);
    begin : blk
      im: assert (req == 0) else $display("im failed");
    end
    #20 $finish;
  end
endmodule
//> 0 assertion system: initialized
//> vpiAssert top.a_req line 13 in top
//> vpiCover top.c_ack line 14 in top
//> vpiImmediateAssert top.blk.im line 30 in top.blk
//> 5 a_req: start (attempt from 5)
//> 5 a_req: vacuous success (attempt from 5)
//> 15 a_req: start (attempt from 15)
//> 25 a_req: start (attempt from 25)
//> 25 a_req: success (attempt from 15)
//> 25 a_req: vacuous success (attempt from 25)
//> 25 c_ack: success (attempt from 25)
//> 35 a_req: start (attempt from 35)
//> 45 a_req: start (attempt from 45)
//> 45 a_req: failure (attempt from 35)
//> 45 a_req: vacuous success (attempt from 45)
//> Error: "39-vpi-assertions.sv", 13: top.a_req: at time 45
//> 55 a_req: start (attempt from 55)
//> 55 a_req: disabled (attempt from 55)
//> 60 a_req: control: disable
//> 80 a_req: control: enable
//> 80 a_req: control: reset
//> in top.blk: vpiImmediateAssert im
//> 80 c_ack: control: disable
//> 80 im: success (attempt from 80)
//> 85 a_req: start (attempt from 85)
//> 85 a_req: vacuous success (attempt from 85)
//> 95 a_req: start (attempt from 95)
//> 95 a_req: vacuous success (attempt from 95)
//> 100 assertion system: end
