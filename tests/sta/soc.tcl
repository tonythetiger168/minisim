create_clock -name clk -period 4 [get_ports clk]
create_clock -name clk2 -period 6 -waveform {1 4} [get_ports clk2]
create_generated_clock -name clkdiv -source [get_ports clk] -divide_by 2 [get_pins div/Q]
set_propagated_clock [get_clocks clk]
set_clock_uncertainty 0.1 [get_clocks clk]
set_clock_uncertainty -hold 0.05 [get_clocks clk]
set_input_delay 1.0 -clock clk [get_ports {a[*] sel}]
set_input_delay 0.2 -clock clk -min [get_ports {a[*] sel}]
set_input_delay 0.5 -clock clk [get_ports rst_n]
set_output_delay 1.5 -clock clk [get_ports {y[0]}]
set_output_delay 1.0 -clock clk2 [get_ports {y[1]}]
set_output_delay 0.5 -clock clkdiv [get_ports z]
set_multicycle_path 2 -setup -from [get_pins ra0/CK] -to [get_pins rb/D]
set_false_path -from [get_ports sel]
set_load 0.02 [get_ports {y[*] z}]
set_input_transition 0.2 [get_ports {a[*]}]
puts "check_timing: [check_timing]"
report_clocks
report_timing -max_paths 20 -path end
report_timing -delay_type min -max_paths 20 -path end
report_timing -to [get_pins rb/D] -input_pins -transition -capacitance
report_timing -to [get_pins rn/D]
report_timing -to [get_pins rc/D]
report_timing -to [get_pins rd/D]
report_timing -to [get_pins rb/RN]
report_timing -delay_type min -to [get_pins rb/RN]
report_constraint -all_violators
report_qor
puts "wns [worst_slack] tns [total_negative_slack]"
