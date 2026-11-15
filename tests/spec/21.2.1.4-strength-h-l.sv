// IEEE 1800-2023 21.2.1.4: %v shows H for "1 or high impedance" and L for
// "0 or high impedance" with one mnemonic, two digits for an unknown whose
// 0 and 1 strengths differ. 28.8, 28.13: a net joined by switches shows the
// strength of the whole group, supply arriving as strong; through a switch
// whose control is unknown a 0 arrives as L and a 1 as H (28.12.1).
module top;
    reg a, en;
    wire h, l, x2, p, q, r, s;
    assign (highz0, pull1) h = a;     // a 1 or nothing
    assign (strong0, highz1) l = a;   // a 0 or nothing
    assign (strong0, pull1) x2 = a;
    assign (supply0, supply1) p = a;
    tran t1(p, q);
    tranif1 t2(p, r, en);
    rtran t3(q, s);
    initial begin
        a = 1'bx; en = 1;
        #1 $display("%v %v %v | %v %v %v %v", h, l, x2, p, q, r, s);
        a = 0; en = 1'bx;
        #1 $display("%v %v %v %v", p, q, r, s);
        a = 1;
        #1 $display("%v %v %v %v", p, q, r, s);
    end
endmodule
//> PuH StL 65X | SuX StX StX PuX
//> Su0 St0 StL Pu0
//> Su1 St1 StH Pu1
