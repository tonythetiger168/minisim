create_clock -name A -period 10 [get_ports aclk]
create_clock -name B -period 7 [get_ports bclk]
create_clock -name C -period 5 [get_ports cclk]
create_clock -name T -period 20 [get_ports tclk]
create_generated_clock -name DIV -source [get_ports aclk] -divide_by 2 [get_pins u_div/q_reg/Q]
create_generated_clock -name U -source [get_ports aclk] -divide_by 2 [get_pins uclk_reg/Q]
set_false_path -from [get_clocks B] -to [get_clocks C]
set_clock_groups -logically_exclusive -group {C} -group {T}
