// IEEE 1800-2023 23.3.3.8: a port connection is sized like an assignment.
// An inout port and the net it connects to that differ in width meet in
// their low bits; the extra bits of the wider side stay unconnected.
module pass2(inout [1:0] a, inout [1:0] b);
    assign a = b;
endmodule
module top;
    reg [2:0] r;
    wire [0:0] n1;
    wire [2:0] n3;
    wire [1:0] o1, o3;
    assign n1 = r;
    assign n3 = r;
    pass2 p1(o1, n1);
    pass2 p3(o3, n3);
    initial begin
        r = 3'b101;
        #1 $display("%b %b %b %b", n1, o1, n3, o3);
        r = 3'b010;
        #1 $display("%b %b %b %b", n1, o1, n3, o3);
    end
endmodule
//> 1 z1 101 01
//> 0 z0 010 10
