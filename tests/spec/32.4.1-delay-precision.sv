// IEEE 1800-2023 32.4.1, 22.7: an SDF delay annotated onto a module path
// is rounded to that module's time precision, as the delays written in
// the module are. 1.134 ns in a 1ns/100ps module is 1.1 ns; 2.26 ns is
// 2.3 ns.
//! icarus-skip: Icarus annotates specify paths only with -gspecify (the same output then)
`timescale 1ns/100ps
module inv(input a, output y);
    assign y = ~a;
    specify
        (a => y) = (0, 0);
    endspecify
endmodule
`timescale 1ns/1ps
module top;
    reg a;
    wire y;
    inv u(.a(a), .y(y));
    initial begin
        $sdf_annotate("data/sdf/32.4.1-delay-precision.sdf", top);
        a = 1;
        #5 a = 0;
        #10 a = 1;
    end
    always @(y) $display("%.3f y=%b", $realtime, y);
endmodule
//> 2.300 y=0
//> 6.100 y=1
//> 17.300 y=0
