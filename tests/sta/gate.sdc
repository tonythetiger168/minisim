create_clock -name clk -period 2 -waveform {0 0.8} [get_ports clk]
set_propagated_clock [get_clocks clk]
set_input_delay 0.3 -clock clk [get_ports {en d}]
set_output_delay 0.3 -clock clk [get_ports q*]
set_clock_gating_check -setup 0.1 -hold 0.05
