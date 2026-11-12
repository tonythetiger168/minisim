# The slow clock is a divided copy of a clock that is not related to F:
# its period, 3 x 4 = 12, comes from the master.
create_clock -name F -period 2 [get_ports fclk]
create_clock -name M -period 3 [get_ports sclk]
create_generated_clock -name S -source [get_ports sclk] -divide_by 4 [get_ports sclk] -add -master_clock M
set_clock_groups -asynchronous -group F -group {M S}
