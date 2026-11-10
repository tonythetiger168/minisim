// A real combinational loop: mequiv must refuse it, not guess.
module loop (input logic a, output logic y);
    logic p, q;
    assign p = a ^ q;
    assign q = ~p;
    assign y = q;
endmodule
