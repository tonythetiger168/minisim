// IEEE 1800-2023 28.16, 29.7: the delays of a bufif / notif gate, a MOS
// switch and a UDP instance may be expressions of variables, read each
// time the output is scheduled to change, as for a continuous assignment.
primitive inv_u(output o, input i);
    table
        0 : 1;
        1 : 0;
    endtable
endprimitive
module top;
    reg in, en;
    integer rise, fall;
    wire b, m, u;
    bufif1 #(rise, fall) g1(b, in, en);
    nmos #(rise, fall) g2(m, in, en);
    inv_u #(rise, fall) g3(u, in);
    initial begin
        rise = 2; fall = 3;
        en = 1; in = 0;
        #10 in = 1;
        #10 rise = 5; fall = 1;
        in = 0;
        #10 in = 1;
    end
    always @(b) $display("%0t b=%b", $time, b);
    always @(m) $display("%0t m=%b", $time, m);
    always @(u) $display("%0t u=%b", $time, u);
endmodule
//> 2 u=1
//> 3 b=0
//> 3 m=0
//> 12 b=1
//> 12 m=1
//> 13 u=0
//> 21 b=0
//> 21 m=0
//> 25 u=1
//> 31 u=0
//> 35 b=1
//> 35 m=1
