create_clock -name clk -period 4 [get_ports clk]
set_propagated_clock [get_clocks clk]
set_input_delay 0.5 -clock clk [get_ports in1]
set_output_delay 0.5 -clock clk [get_ports out*]
set_load 0.01 [get_ports out*]
set_timing_derate -early 0.9
set_timing_derate -late 1.2
