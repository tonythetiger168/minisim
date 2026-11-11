create_clock -name clk -period 2 [get_ports clk]
set_input_delay 0.5 -clock clk [get_ports in1]
set_output_delay 0.4 -clock clk [get_ports out1]
set_load 0.01 [get_ports out1]
