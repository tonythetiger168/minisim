create_clock -name clk -period 2 [get_ports clk]
set_input_delay 0.1 -clock clk [get_ports {a b sel en}]
report_timing -to [get_pins rc/D] -path end
set_case_analysis 0 [get_ports sel]
report_timing -to [get_pins rc/D]
set_case_analysis 1 [get_ports en]
report_timing -to [get_pins rc/D] -path end
report_timing -delay_type min -to [get_pins rc/D] -path end
check_timing
