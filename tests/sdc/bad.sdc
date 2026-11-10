# Every msdc rule fires at least once on soc.sv with this file.
create_clock -name clk_a -period 10 [get_ports clk_a]
create_clock -name clk_b -period 7 -waveform {5 2} [get_ports clk_b]
create_clock -name clk_b2 -period 9 [get_ports clk_b]
create_clock -name clk_a -period 12 [get_ports clk_a]
create_clock -name zero -period 0
create_clock -name vfree -period 5
create_clock -name scan_clk -period 100 [get_ports scan_clk]
create_generated_clock -name div2 -source [get_ports clk_a] -divide_by 2 [get_pins div2_reg/Q]
create_generated_clock -name gcopy -source [get_ports rst_n] [get_pins u_core/flag_reg/Q]
create_generated_clock -name gbad -source [get_ports clk_a] -divide_by 2 -edges {1 3} [get_pins u_core/flag_reg/Q] -add

set_clock_groups -asynchronous -group {clk_a} -group {div2 clk_a}
set_clock_groups -group {clk_b2}
set_clock_groups -asynchronous -group [get_clocks nope_clk] -group clk_a

set_input_delay -clock clk_a 11 [get_ports din]
set_input_delay -clock clk_a -min 3 [get_ports en]
set_input_delay -clock clk_a -max 1 [get_ports en]
set_input_delay 2 [get_ports b_in]
set_input_delay -clock clk_b2 1 [get_ports dout]
set_input_delay -clock clk_a 1 [get_ports clk_b]
set_output_delay -clock clk_a 1 [get_ports flag_b]
set_output_delay -clock clk_b2 2 [get_ports flag_b]
set_output_delay -clock clk_a 1 [get_ports en]
set_output_delay -clock missing_clk 1 [get_ports div_out]

set_false_path -from [get_ports no_such_port] -to [get_ports dout]
set_false_path -from [get_ports dout] -to [get_ports din]
set_false_path -from [get_pins u_core/flag_reg/D] -to [get_pins u_core/acc_reg[0]/Q]
set_false_path -from [get_cells u_sync/q_reg] -to [get_cells u_core/flag_reg]
set_multicycle_path 3 -setup -from [get_cells u_core/acc_reg*] -to [get_cells u_core/flag_reg]
set_multicycle_path 0 -from [get_cells div2_reg]
set_max_delay 4 -from [get_ports en] -to [get_ports comb_out]
set_max_delay 6 -from [get_ports en] -to [get_ports comb_out]
set_min_delay 9 -from [get_ports en] -to [get_ports comb_out]
set_false_path -from [get_ports en] -to [get_ports comb_out]
set_false_path

set_case_analysis 0 [get_ports test_mode]
set_case_analysis 1 [get_ports test_mode]
set_case_analysis 2 [get_ports rst_n]

set_dont_touch [get_cells u_core]
set_load -bogus 1 [get_ports dout]
set_load 0.1 [get_ports dout
