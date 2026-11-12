# The same constraints as cdc_sdc_groups.sdc, written the other ways SDC
# allows: variables, loops, bare names, wildcards, -hierarchical pins,
# get_clocks, and commands that do not change a domain.
set_units -time ns
set period(A) 10
set period(B) 7
foreach {name port} {A aclk B bclk C cclk T tclk} {
    create_clock -name $name -period $period(A) [get_ports $port]
}
create_generated_clock -name DIV -source aclk -divide_by 2 [get_pins -hierarchical q_reg/Q]
create_generated_clock -name U -master_clock [get_clocks A] -source [get_ports aclk] -divide_by 2 uclk_reg/Q
set_clock_uncertainty 0.1 [all_clocks]
set_input_transition 0.2 [all_inputs]
set_load 0.01 [all_outputs]
set_multicycle_path 2 -from [get_clocks A] -to [get_clocks B]
set_clock_groups -asynchronous -group [get_clocks {A}] -group [get_clocks B*]
set_clock_groups -physically_exclusive -group C -group T
set_case_analysis 0 test_mode
