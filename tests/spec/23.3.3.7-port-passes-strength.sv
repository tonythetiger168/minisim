// IEEE 1800-2023 23.3.3.7: nets joined through a port act as one net, so
// a driver's strength reaches the other side of the port (28.12). A gate
// declared (strong0, pull1) behind an output port loses to a strong 0
// outside it; a (weak0, weak1) driver of an input port loses to a pull
// driver inside (whether the input is also coerced to an inout,
// 23.3.3.6, is not shown). A delayed gate's x keeps its strength range (28.12.2).
module drv_pull(output y, input a);
    not (strong0, pull1) #2 g(y, a);
endmodule
module sink(input i, output o);
    assign (pull0, pull1) i = 1'b0;
    assign o = i;
endmodule
module top;
    reg a;
    wire y, w, o;
    drv_pull d(.y(y), .a(a));
    assign (strong0, strong1) y = 1'b0;
    assign (weak0, weak1) w = 1'b1;
    sink s(.i(w), .o(o));
    initial begin
        #1 $display("%b %v", y, y);
        a = 0;
        #3 $display("%b %v", y, y);
        $display("%v %b", s.i, o);
    end
endmodule
//> 0 St0
//> 0 St0
//> Pu0 0
