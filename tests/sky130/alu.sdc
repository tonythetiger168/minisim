create_clock -name clk -period 2 [get_ports clk]
set_input_delay 0.2 -clock clk [all_inputs]
set_output_delay 0.2 -clock clk [all_outputs]
