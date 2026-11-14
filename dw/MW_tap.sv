// MW_tap - IEEE 1149.1 test access port controller.
//   The 16-state TAP state machine on tck/tms (trst_n resets it
//   asynchronously, as do five tck cycles with tms high), an instruction
//   register of IR_WIDTH bits that captures 2'b01 in its low bits, and two
//   data registers of its own: BYPASS (one bit, capturing 0) and the
//   32-bit IDCODE (IDCODE, bit 0 forced to 1), selected by the IDCODE
//   instruction (the one after reset) and the all-ones BYPASS instruction.
//   Any other instruction selects the user data register: the controller
//   gives the shift/capture/update strobes (shift_dr, capture_dr,
//   update_dr), the instruction (ir), and passes user_tdo to tdo. tdo
//   changes on the falling edge of tck and tdo_en is high while shifting.
module MW_tap #(
    parameter int              IR_WIDTH = 4,
    parameter logic [31:0]     IDCODE   = 32'h0000_0001,
    parameter logic [IR_WIDTH-1:0] IDCODE_OPCODE = IR_WIDTH'(1)
) (
    input  logic                tck,
    input  logic                trst_n,
    input  logic                tms,
    input  logic                tdi,
    output logic                tdo,
    output logic                tdo_en,
    output logic [IR_WIDTH-1:0] ir,
    output logic                test_logic_reset,
    output logic                capture_dr,
    output logic                shift_dr,
    output logic                update_dr,
    input  logic                user_tdo
);
    typedef enum logic [3:0] {
        TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PAU_DR, EX2_DR, UPD_DR,
        SEL_IR, CAP_IR, SH_IR, EX1_IR, PAU_IR, EX2_IR, UPD_IR
    } state_t;
    state_t st, nx;
    always_comb begin
        case (st)
            TLR:    nx = tms ? TLR    : RTI;
            RTI:    nx = tms ? SEL_DR : RTI;
            SEL_DR: nx = tms ? SEL_IR : CAP_DR;
            CAP_DR: nx = tms ? EX1_DR : SH_DR;
            SH_DR:  nx = tms ? EX1_DR : SH_DR;
            EX1_DR: nx = tms ? UPD_DR : PAU_DR;
            PAU_DR: nx = tms ? EX2_DR : PAU_DR;
            EX2_DR: nx = tms ? UPD_DR : SH_DR;
            UPD_DR: nx = tms ? SEL_DR : RTI;
            SEL_IR: nx = tms ? TLR    : CAP_IR;
            CAP_IR: nx = tms ? EX1_IR : SH_IR;
            SH_IR:  nx = tms ? EX1_IR : SH_IR;
            EX1_IR: nx = tms ? UPD_IR : PAU_IR;
            PAU_IR: nx = tms ? EX2_IR : PAU_IR;
            EX2_IR: nx = tms ? UPD_IR : SH_IR;
            default: nx = tms ? SEL_DR : RTI;       // UPD_IR
        endcase
    end
    always_ff @(posedge tck or negedge trst_n)
        if (!trst_n) st <= TLR;
        else         st <= nx;

    logic [IR_WIDTH-1:0] irs;          // instruction shift register
    logic [31:0]         idr;          // IDCODE register
    logic                byp;
    localparam logic [IR_WIDTH-1:0] BYPASS = '1;
    always_ff @(posedge tck or negedge trst_n) begin
        if (!trst_n) begin
            irs <= '0;
            ir  <= IDCODE_OPCODE;
            idr <= '0;
            byp <= 1'b0;
        end else begin
            case (st)
                TLR:    ir  <= IDCODE_OPCODE;
                CAP_IR: irs <= IR_WIDTH'(2'b01);
                SH_IR:  irs <= {tdi, irs[IR_WIDTH-1:1]};
                UPD_IR: ir  <= irs;
                CAP_DR: begin idr <= IDCODE | 32'd1; byp <= 1'b0; end
                SH_DR:  begin idr <= {tdi, idr[31:1]}; byp <= tdi; end
                default: ;
            endcase
        end
    end
    assign test_logic_reset = st == TLR;
    assign capture_dr = st == CAP_DR && ir != BYPASS && ir != IDCODE_OPCODE;
    assign shift_dr   = st == SH_DR  && ir != BYPASS && ir != IDCODE_OPCODE;
    assign update_dr  = st == UPD_DR && ir != BYPASS && ir != IDCODE_OPCODE;

    // tdo: the low bit of the selected register, on the falling edge
    logic so;
    always_comb
        if (st == SH_IR) so = irs[0];
        else if (ir == BYPASS) so = byp;
        else if (ir == IDCODE_OPCODE) so = idr[0];
        else so = user_tdo;
    always_ff @(negedge tck or negedge trst_n)
        if (!trst_n) begin
            tdo    <= 1'b0;
            tdo_en <= 1'b0;
        end else begin
            tdo    <= so;
            tdo_en <= st == SH_IR || st == SH_DR;
        end
endmodule
