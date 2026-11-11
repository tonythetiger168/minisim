# Constraints with nothing to report on soc.sv.
set_units -time ns
set pa 10.0
create_clock -name clk_a -period $pa [get_ports clk_a]
create_clock -name clk_b -period 7 [get_ports clk_b]
create_clock -name clk_x -period 20 [get_ports clk_x]
create_clock -name scan_clk -period 100 [get_ports scan_clk]
create_generated_clock -name div2 -source [get_ports clk_a] -divide_by 2 [get_pins div2_reg/Q]

set_clock_groups -asynchronous -group {clk_a div2} -group {clk_b scan_clk} -group {clk_x} -name async
set_clock_groups -physically_exclusive -group clk_b -group scan_clk
set_clock_uncertainty 0.2 [all_clocks]

set_input_delay -clock clk_a -max [expr {$pa * 0.4}] [get_ports {din en}]
set_input_delay -clock clk_a -min 0.5 [get_ports {din en}]
set_input_delay -clock clk_b 2 [get_ports b_in]
set_output_delay -clock clk_a 3 [get_ports {dout div_out}]
set_output_delay -clock clk_b 2 [get_ports {flag_b comb_out}]
set_output_delay -clock clk_x 2 [get_ports x_out]
set_false_path -from [get_ports rst_n]
set_case_analysis 0 [get_ports test_mode]

set_multicycle_path 2 -setup -from [get_cells u_core/acc_reg*] -to [get_cells u_core/flag_reg]
set_multicycle_path 1 -hold  -from [get_cells u_core/acc_reg*] -to [get_cells u_core/flag_reg]
set_max_delay 5 -from [get_ports en] -to [get_ports comb_out]

set_driving_cell -lib_cell BUFX2 [all_inputs]
set_load 0.05 [all_outputs]
