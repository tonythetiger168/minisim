create_clock -name CLK -period 10 [get_ports clk]
create_clock -name VIO -period 25
set_clock_groups -asynchronous -group {CLK} -group {VIO}
set_input_delay -clock CLK 2 [get_ports din_a]
set_input_delay -clock VIO 3 [get_ports {din_v din_s}]
set_output_delay -clock CLK 2 [get_ports dout_a]
set_output_delay -clock VIO 4 [get_ports dout_v]
