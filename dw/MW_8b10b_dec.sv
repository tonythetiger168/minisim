// MW_8b10b_dec - 8b/10b decoder, the inverse of MW_8b10b_enc.
//   code is abcdei fghj (a in bit 9), rd_in the running disparity before
//   it. data and k give the character; code_err flags a code that is no
//   character at either disparity, rd_err one that is a character but not
//   at rd_in's disparity. rd_out follows the code's own disparity (as a
//   receiver does, so one error does not repeat).
//   Built from MW_8b10b_enc instances with constant inputs: each folds to
//   the constant code of one character at one disparity.
module MW_8b10b_dec (
    input  logic [9:0] code,
    input  logic       rd_in,
    output logic [7:0] data,
    output logic       k,
    output logic       rd_out,
    output logic       code_err,
    output logic       rd_err
);
    localparam int N = 268;     // 256 data and 12 control characters
    logic [9:0] tab [2][N];
    for (genvar p = 0; p < 2; p++) begin : g_rd
        for (genvar i = 0; i < N; i++) begin : g_ch
            localparam logic       KC = i >= 256;
            localparam logic [7:0] D  = i < 256 ? 8'(i) : i < 264 ? {3'(i - 256), 5'd28}
                                      : i == 264 ? {3'd7, 5'd23} : i == 265 ? {3'd7, 5'd27}
                                      : i == 266 ? {3'd7, 5'd29} : {3'd7, 5'd30};
            logic ro, ke;
            MW_8b10b_enc u (.k(KC), .data(D), .rd_in(p[0]), .code(tab[p][i]), .rd_out(ro), .k_err(ke));
        end
    end
    logic r, found;
    always_comb begin
        data = '0;
        k = 1'b0;
        found = 1'b0;
        rd_err = 1'b0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < N; i++)
                if (!found && tab[rd_in ^ pass[0]][i] == code) begin
                    found = 1'b1;
                    k = i >= 256;
                    data = i < 256 ? 8'(i) : i < 264 ? {3'(i - 256), 5'd28}
                         : i == 264 ? {3'd7, 5'd23} : i == 265 ? {3'd7, 5'd27}
                         : i == 266 ? {3'd7, 5'd29} : {3'd7, 5'd30};
                    rd_err = pass != 0;
                end
        code_err = !found;
        // the disparity after it: an unbalanced sub-block sets it
        r = rd_in;
        if ($countones(code[9:4]) != 3) r = $countones(code[9:4]) > 3;
        if ($countones(code[3:0]) != 2) r = $countones(code[3:0]) > 2;
        rd_out = r;
    end
endmodule
