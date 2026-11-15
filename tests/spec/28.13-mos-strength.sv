// IEEE 1800-2023 28.13 and 28.14: nmos, pmos and cmos pass the strength of
// their data input, except that supply becomes strong; rnmos, rpmos and
// rcmos reduce it (Table 28-8). When the data net keeps its value but loses
// its strong driver, the switch output follows the new strength.
//! icarus-skip: Icarus 13 does not reduce strength through rnmos (Table 28-8)
module top;
    wire w, q, r, s, rs, c;
    supply1 vdd;
    reg g;
    pullup(w);
    bufif1(w, 1'b1, g);       // w: St1 while g is 1, else Pu1
    pmos(q, w, 1'b0);         // q follows w's strength
    bufif0(q, 1'b0, g);       // St0 on q once g is 0: beats Pu1, not St1
    nmos(s, vdd, 1'b1);       // supply in, strong out
    rnmos(rs, vdd, 1'b1);     // resistive: supply in, pull out
    rnmos(r, w, 1'b1);        // resistive: Pu1 in, We1 out
    cmos(c, w, 1'b1, 1'b0);
    initial begin
        g = 1;
        #1 $display("%v %v %v %v %v", w, q, s, rs, c);
        g = 0;
        #1 $display("%v %v %v %v %b", w, q, r, c, q);
    end
endmodule
//> St1 St1 St1 Pu1 St1
//> Pu1 St0 We1 Pu1 0
