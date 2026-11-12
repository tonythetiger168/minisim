// CDC009: pulses from a fast clock into a slow one.
module cdc_pulse (input logic fclk, sclk, rst_n, go, input logic [3:0] cnt);
  // A pulse: the flop takes a new value every cycle.
  logic pulse_f;
  always_ff @(posedge fclk or negedge rst_n)
    if (!rst_n) pulse_f <= 1'b0; else pulse_f <= go & ~cnt[0];
  logic [1:0] pulse_s;
  always_ff @(posedge sclk or negedge rst_n)
    if (!rst_n) pulse_s <= '0; else pulse_s <= {pulse_s[0], pulse_f};

  // A toggle (pulse synchronizer): the flop holds or flips itself.
  logic tog_f;
  always_ff @(posedge fclk or negedge rst_n)
    if (!rst_n) tog_f <= 1'b0; else if (go) tog_f <= ~tog_f;
  logic [2:0] tog_s;
  always_ff @(posedge sclk or negedge rst_n)
    if (!rst_n) tog_s <= '0; else tog_s <= {tog_s[1:0], tog_f};

  // Slow to fast: a pulse of the slow clock lasts several fast cycles.
  logic back_s;
  always_ff @(posedge sclk or negedge rst_n)
    if (!rst_n) back_s <= 1'b0; else back_s <= pulse_s[1];
  logic [1:0] back_f;
  always_ff @(posedge fclk or negedge rst_n)
    if (!rst_n) back_f <= '0; else back_f <= {back_f[0], back_s};
endmodule
