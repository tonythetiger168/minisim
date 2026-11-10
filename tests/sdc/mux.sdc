# A clock mux with nothing said about its two clocks, an input with no delay,
# and all_registers -clock.
create_clock -name clk_a -period 10 [get_ports clk_a]
create_clock -name clk_b -period 7 [get_ports clk_b]
create_clock -name clk_x -period 20 [get_ports clk_x]
create_clock -name scan_clk -period 100 [get_ports scan_clk]
create_generated_clock -name div2 -source [get_ports clk_a] -divide_by 2 [get_pins div2_reg/Q]
set_clock_uncertainty 0.1 [all_clocks]
set_clock_groups -asynchronous -group {clk_a div2} -group {clk_b scan_clk} -group clk_x
set_input_delay -clock clk_a 2 [get_ports {din en}]
set_output_delay -clock clk_a 2 [all_outputs]
set_false_path -from [get_ports rst_n]
puts "clk_b registers: [sizeof_collection [all_registers -clock clk_b]]"
puts "data pins: [sizeof_collection [all_registers -data_pins -clock [get_clocks clk_a]]]"
set_false_path -to [all_registers -clock clk_x]
