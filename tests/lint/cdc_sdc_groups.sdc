create_clock -name A -period 10 [get_ports aclk]
create_clock -name B -period 7 [get_ports bclk]
create_clock -name C -period 5 [get_ports cclk]
create_clock -name T -period 20 [get_ports tclk]
create_generated_clock -name DIV -source [get_ports aclk] -divide_by 2 [get_pins u_div/q_reg/Q]
create_generated_clock -name U -source [get_ports aclk] -divide_by 2 [get_pins uclk_reg/Q]
# A (with DIV and U, which follow their master) against B. C is in no group:
# synchronous to both, as it is for timing.
set_clock_groups -asynchronous -group {A} -group {B}
set_clock_groups -physically_exclusive -group {C} -group {T}
# test_mode is 0 in function: the clock mux is not switched.
set_case_analysis 0 [get_ports test_mode]
