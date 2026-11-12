create_clock -name F -period 2 [get_ports fclk]
create_clock -name S -period 10 [get_ports sclk]
set_clock_groups -asynchronous -group F -group S
