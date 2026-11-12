# CDC model of cdc_block, written by mlint -cdc-abstract.
# Use it with -cdc-model in place of the module's RTL.
module cdc_block
  input  clk_a  clock
  input  clk_b  clock
  input  d  data clk_a
  input  req  sync clk_b
  output q  data clk_b
  output ack  data clk_a
  output y  comb
end
