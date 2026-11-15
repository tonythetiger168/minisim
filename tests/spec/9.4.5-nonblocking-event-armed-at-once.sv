// IEEE 1800-2023 9.4.5: in `a <= @(e) b` the wait for e starts when the
// statement executes. An event triggered by the same process right after it,
// in the same time step, therefore schedules the update, which then lands in
// the NBA region of that step (4.4.2). Part selects work the same way.
//! icarus-skip: Icarus 13 writes an event-controlled nonblocking update as soon as the event fires
module top;
    reg [3:0] x, y;
    event e;
    initial begin
        x = 'x; y = 'x;
        x <= @e 4'b1010;
        y[2:1] <= @e 2'b01;
        -> e;
        $display("now %b %b", x, y);
        #0 $display("inactive %b %b", x, y);
        #1 $display("next %b %b", x, y);
    end
endmodule
//> now xxxx xxxx
//> inactive xxxx xxxx
//> next 1010 x01x
