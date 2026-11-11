create_clock -name clk -period 0.7 [get_ports clk]
set_input_delay 0.1 -clock clk [get_ports d*]
set_output_delay 0.1 -clock clk [get_ports q*]
