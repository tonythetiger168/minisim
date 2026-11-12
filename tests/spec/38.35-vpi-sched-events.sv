// IEEE 1800-2023 38.35, 38.36: a delayed vpi_put_value with vpiReturnEvent
// gives a vpiSchedEvent handle (vpiScheduled until it lands) that
// vpiCancelEvent cancels; an inertial write removes the writes pending on
// the object, a transport write those due after it, a pure transport write
// none. cbForce / cbRelease / cbAssign / cbDeassign report force and
// procedural assign (on one object or, obj NULL, on all), cbNBASynch runs
// before the NBA updates of its slot, cbAtEndOfSimTime at the end of it.
//! icarus-skip: Icarus 13 has no cbNBASynch or cbAtEndOfSimTime
//! args: data/vpi/schedevents.c
module top;
  logic [7:0] a = 0, b = 0, c = 0;
  wire [7:0] w = c;
  logic [7:0] q = 0;
  initial begin
    $sched;              // at 0: a <= 1 at 10 (event), cancelled at 5; b inertial / transport
    #5 $cancel;
    #20 $show;
    $holds;
    force a = 8'h55;
    #1 release a;
    force w = 8'haa;
    #1 release w;
    assign c = 8'h0f;
    #1 deassign c;
    #1 q <= 8'h11;
    $slot;
    #1 $display("q %h", q);
  end
endmodule
//> event type 53 scheduled 1
//> @5 cancelled: scheduled 0
//> @25 a 0 b 2 c 5
//> @25 force top.a = 55 (any)
//> @26 release top.a = 55 (any)
//> @26 force top.w = aa (any)
//> @26 force top.w = aa (w)
//> @27 release top.w = 5 (any)
//> @27 assign top.c = f (c)
//> @28 deassign top.c = f (c)
//> @29 cbNBASynch: q 0
//> @29 cbAtEndOfSimTime: q 11
//> q 11
