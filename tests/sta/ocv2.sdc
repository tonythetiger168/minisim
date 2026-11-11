source ocv.sdc
set_timing_derate -late 1.5 -cell_delay [get_cells cb1]
set_timing_derate -early 0.8 -cell_delay -data [get_lib_cells */XOR2_X1]
set_timing_derate -late 1.1 -cell_check
set_timing_derate -early 1.3 -cell_check
