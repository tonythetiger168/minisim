// MW_8b10b_enc - 8b/10b encoder (IEEE 802.3 clause 36, Widmer-Franaszek).
//   data = HGFEDCBA, k for a control character. code is abcdei fghj with
//   a in bit 9, the bit sent first. rd_in is the running disparity before
//   the character (0 negative, 1 positive), rd_out after it; keep it in a
//   register between characters. The valid control characters are K28.0
//   to K28.7, K23.7, K27.7, K29.7 and K30.7; k_err flags any other (its
//   code is then the data character's).
module MW_8b10b_enc (
    input  logic       k,
    input  logic [7:0] data,
    input  logic       rd_in,
    output logic [9:0] code,
    output logic       rd_out,
    output logic       k_err
);
    // 5b/6b for RD- (abcdei); RD+ takes the complement when unbalanced
    function automatic logic [5:0] six(input logic [4:0] x);
        case (x)
            5'd0:  six = 6'b100111;  5'd1:  six = 6'b011101;  5'd2:  six = 6'b101101;  5'd3:  six = 6'b110001;
            5'd4:  six = 6'b110101;  5'd5:  six = 6'b101001;  5'd6:  six = 6'b011001;  5'd7:  six = 6'b111000;
            5'd8:  six = 6'b111001;  5'd9:  six = 6'b100101;  5'd10: six = 6'b010101;  5'd11: six = 6'b110100;
            5'd12: six = 6'b001101;  5'd13: six = 6'b101100;  5'd14: six = 6'b011100;  5'd15: six = 6'b010111;
            5'd16: six = 6'b011011;  5'd17: six = 6'b100011;  5'd18: six = 6'b010011;  5'd19: six = 6'b110010;
            5'd20: six = 6'b001011;  5'd21: six = 6'b101010;  5'd22: six = 6'b011010;  5'd23: six = 6'b111010;
            5'd24: six = 6'b110011;  5'd25: six = 6'b100110;  5'd26: six = 6'b010110;  5'd27: six = 6'b110110;
            5'd28: six = 6'b001110;  5'd29: six = 6'b101110;  5'd30: six = 6'b011110;  default: six = 6'b101011;
        endcase
    endfunction
    // 3b/4b for a negative running disparity at the 4b sub-block (fghj)
    function automatic logic [3:0] four(input logic [2:0] y, input logic kc, input logic alt);
        if (kc)
            case (y)
                3'd0: four = 4'b1011;  3'd1: four = 4'b0110;  3'd2: four = 4'b1010;  3'd3: four = 4'b1100;
                3'd4: four = 4'b1101;  3'd5: four = 4'b0101;  3'd6: four = 4'b1001;  default: four = 4'b0111;
            endcase
        else
            case (y)
                3'd0: four = 4'b1011;  3'd1: four = 4'b1001;  3'd2: four = 4'b0101;  3'd3: four = 4'b1100;
                3'd4: four = 4'b1101;  3'd5: four = 4'b1010;  3'd6: four = 4'b0110;  default: four = alt ? 4'b0111 : 4'b1110;
            endcase
    endfunction
    logic [4:0] x;
    logic [2:0] y;
    logic       k28, kx7, kc;
    logic [5:0] s;
    logic [3:0] f;
    logic       rd6, alt;
    always_comb begin
        x = data[4:0];
        y = data[7:5];
        k28 = k && x == 5'd28;
        kx7 = k && y == 3'd7 && (x == 5'd23 || x == 5'd27 || x == 5'd29 || x == 5'd30);
        k_err = k && !k28 && !kx7;
        kc = k28;
        // 6b: K28's own code, else the data code; flip on RD+ unless balanced
        // (D.07 is balanced and still alternates)
        s = k28 ? 6'b001111 : six(x);
        if (rd_in && ($countones(s) != 3 || s == 6'b111000)) s = ~s;
        rd6 = $countones(s) == 3 ? rd_in : $countones(s) > 3;
        // D.x.A7 keeps runs to five: RD- with x = 17, 18, 20; RD+ with 11, 13, 14
        alt = kx7 || (!rd6 && (x == 5'd17 || x == 5'd18 || x == 5'd20)) || (rd6 && (x == 5'd11 || x == 5'd13 || x == 5'd14));
        f = four(y, kc, alt);
        if (rd6 && ($countones(f) != 2 || f == 4'b1100 || (kc && (y == 3'd1 || y == 3'd2 || y == 3'd5 || y == 3'd6))))
            f = ~f;
        rd_out = $countones(f) == 2 ? rd6 : $countones(f) > 2;
        code = {s, f};
    end
endmodule
