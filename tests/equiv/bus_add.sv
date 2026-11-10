// bus.sv written with a plain adder.
module bus (input logic [3:0] a, b, input logic cin, output logic [3:0] s, output logic cout, output logic v, w);
    assign {cout, s} = a + b + cin;
    assign w = a[0] & b[0];
    assign v = ~w | a[1];
endmodule
