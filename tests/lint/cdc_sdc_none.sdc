# Clocks only: nothing says how they are related.
create_clock -name A -period 10 [get_ports aclk]
create_clock -name B -period 7 [get_ports bclk]
create_clock -name C -period 5 [get_ports cclk]
create_clock -name T -period 20 [get_ports tclk]
create_generated_clock -name DIV -source [get_ports aclk] -divide_by 2 [get_pins u_div/q_reg/Q]
