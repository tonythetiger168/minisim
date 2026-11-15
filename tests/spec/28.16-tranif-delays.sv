// IEEE 1800-2023 28.16: a tranif switch's first delay is its turn-on delay
// and its second its turn-off delay: the terminals join that long after the
// control turns the switch on, and part that long after it turns it off.
// The delays may be expressions of variables. For tranif0 turning on is its
// control going to 0.
//! icarus-skip: Icarus 13 uses a tranif0's first delay for its control's rise (turn-off), not for turn-on
module top;
    reg a, en;
    integer off;
    wire b1, b0;
    assign a_n = a;
    tranif1 #(3, off) s1(a_n, b1, en);
    tranif0 #(2, 4) s0(a_n, b0, en);
    initial begin
        off = 5;
        a = 1;
        en = 0;
        #10 en = 1;
        #10 en = 0;
        #10 $finish;
    end
    always @(b1) if ($time > 0) $display("%0t b1=%b", $time, b1);
    always @(b0) if ($time > 0) $display("%0t b0=%b", $time, b0);
endmodule
//> 2 b0=1
//> 5 b1=z
//> 13 b1=1
//> 14 b0=z
//> 22 b0=1
//> 25 b1=z
