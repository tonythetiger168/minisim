create_clock -name clk -period 4 [get_ports clk]
create_clock -name clk2 -period 6 -waveform {1 4} [get_ports clk2]
create_clock -name vclk -period 4
set_input_delay 1.0 -clock vclk [get_ports {a[*] sel}]
set_output_delay 1.5 -clock vclk [get_ports {y[*] z}]
set_clock_groups -asynchronous -group {clk vclk} -group clk2
set_false_path -through [get_pins x1/Y] -through [get_pins m1/Y]
set_max_delay 0.3 -from [get_cells ra0] -to [get_pins rd/D]
set_multicycle_path 3 -setup -from [get_clocks clk] -to [get_pins rn/D]
set_multicycle_path 2 -hold -from [get_clocks clk] -to [get_pins rn/D]
set_clock_latency 0.3 [get_clocks clk]
set_clock_latency -source 0.2 [get_clocks clk]
set_driving_cell -lib_cell BUF_X1 [get_ports sel]
set_max_transition 0.03 [current_design]
set_disable_timing [get_cells di]
set_max_capacitance 0.004 [get_pins ra1/Q]
check_timing -verbose
report_timing -max_paths 20 -path end
report_timing -delay_type min -max_paths 20 -path end
report_timing -to [get_pins rd/D]
report_timing -to [get_pins rn/D]
report_timing -delay_type min -to [get_pins rn/D]
report_timing -from [get_ports sel] -to [get_pins rb/D] -input_pins
report_constraint -all_violators
