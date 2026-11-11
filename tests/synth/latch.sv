// Not synthesizable by msynth: y holds its value when en is 0.
module latch (input logic en, d, output logic y);
    always_comb
        if (en) y = d;
endmodule
