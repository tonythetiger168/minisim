// A reference design for mequiv; fsm_net.v is its synthesized netlist.
module fsm(input clk, input rst_n, input go, input [3:0] din, output reg [3:0] dout, output reg busy, output done);
  typedef enum logic [1:0] {IDLE, LOAD, RUN, FIN} st_t;
  st_t st, nx;
  reg [3:0] acc; reg [2:0] n;
  always_comb begin
    nx = st; 
    case (st)
      IDLE: if (go) nx = LOAD;
      LOAD: nx = RUN;
      RUN:  if (n == 0) nx = FIN;
      FIN:  nx = IDLE;
    endcase
  end
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin st <= IDLE; acc <= 0; n <= 0; dout <= 0; busy <= 0; end
    else begin
      st <= nx;
      busy <= (nx != IDLE);
      case (st)
        LOAD: begin acc <= din; n <= 3'd5; end
        RUN: begin acc <= {acc[2:0], acc[3] ^ acc[0]}; n <= n - 1; end
        FIN: dout <= acc;
        default: ;
      endcase
    end
  assign done = st == FIN;
endmodule
