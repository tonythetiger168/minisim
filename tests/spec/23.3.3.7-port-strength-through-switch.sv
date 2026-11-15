// IEEE 1800-2023 23.3.3.7, 28.8: a one-bit input port on a bit of a net
// that is joined to other nets (here through inout ports on bit and part
// selects) passes on the strength of the joined group's drivers, not only
// of the net's own. A cell drives the pad from two levels down while the
// testbench lets go of it; a reader on the top-level pad bit still sees
// the driven value, never z.
module tbuf(output y, input a, en);
    bufif1 (y, a, en);
endmodule
module rd(input d, output q);
    assign q = d;
endmodule
module pcell(input oe, a, inout pad);
    tbuf u(.y(pad), .a(a), .en(oe));
endmodule
module bank(input oe, input [1:0] a, inout [1:0] pads);
    pcell c0(.oe(oe), .a(a[0]), .pad(pads[0]));
    pcell c1(.oe(oe), .a(a[1]), .pad(pads[1]));
endmodule
module chip(input oe, input [1:0] a, inout [3:0] pad, output [3:0] q);
    bank lo(.oe(oe), .a(a), .pads(pad[1:0]));
    rd r0(.d(pad[0]), .q(q[0]));
    rd r1(.d(pad[1]), .q(q[1]));
    rd r2(.d(pad[2]), .q(q[2]));
    rd r3(.d(pad[3]), .q(q[3]));
endmodule
module top;
    reg oe;
    reg [1:0] a;
    reg [3:0] ext;
    wire [3:0] pad, q;
    assign pad[1:0] = oe ? 2'bz : ext[1:0];
    assign pad[3:2] = ext[3:2];
    chip dut(.oe(oe), .a(a), .pad(pad), .q(q));
    initial begin
        oe = 0; a = 2'b01; ext = 4'b1010;
        #1 $display("%b %b", pad, q);
        oe = 1;
        #1 $display("%b %b", pad, q);
        a = 2'b10;
        #1 $display("%b %b", pad, q);
        oe = 0; ext = 4'b0111;
        #1 $display("%b %b", pad, q);
    end
endmodule
//> 1010 1010
//> 1001 1001
//> 1010 1010
//> 0111 0111
