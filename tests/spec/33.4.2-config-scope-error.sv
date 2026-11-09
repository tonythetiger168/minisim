// IEEE 1800-2023 33.4.2: once an instance is bound to a config (use
// lib.cell:config), only that config's rules apply below it. An instance
// clause of the outer config that reaches into it is an error.
//! icarus-skip: Icarus 13 does not support configurations
//! exit: 1
module a1m; endmodule
module bot; a1m a1(); endmodule
module top; bot bot(); endmodule

config bot;
  design work.bot;
  default liblist work;
  instance bot.a1 liblist work;
endconfig

config top;
  design work.top;
  default liblist work;
  instance top.bot use work.bot:config;
  instance top.bot.a1 liblist work;
  // ERROR - cannot set liblist for top.bot.a1 from this config
endconfig
