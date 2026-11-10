# msdc self-test: nothing fires on tests/sdc/clean.sdc, every rule fires on
# bad.sdc and mux.sdc, and the report does not depend on the thread count.
#   cmake -DMSDC=<msdc> -DSRC=<source dir> -DOUT=<scratch dir> -P msdc.cmake
function(run sdc want_rc)
  execute_process(COMMAND ${MSDC} ${SRC}/tests/sdc/soc.sv -top soc -sdc ${SRC}/tests/sdc/${sdc} ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  if(NOT rc EQUAL ${want_rc})
    message(FATAL_ERROR "msdc ${sdc} exited ${rc}, expected ${want_rc}\n${out}${err}")
  endif()
endfunction()

run(clean.sdc 0)
if(NOT out MATCHES "msdc: 0 findings" OR NOT out MATCHES "17 of 17 register bits are clocked")
  message(FATAL_ERROR "clean.sdc is not clean:\n${out}")
endif()

run(bad.sdc 1)
set(bad "${out}")
foreach(want
    "bad.sdc:45: error: [SDC001] set_load: unknown option -bogus"
    "bad.sdc:46: error: [SDC001] missing close-bracket"
    "bad.sdc:44: error: [SDC002] unknown command 'set_dont_touch'"
    "bad.sdc:28: warning: [SDC003] no port matches 'no_such_port'"
    "bad.sdc:26: warning: [SDC004] nothing matches 'missing_clk'"
    "bad.sdc:5: warning: [SDC010] clock 'clk_a' is defined again"
    "bad.sdc:4: warning: [SDC011] clock 'clk_b2' is on clk_b, which already has clock 'clk_b'"
    "bad.sdc:3: warning: [SDC012] clock 'clk_b': the -waveform edges must increase"
    "bad.sdc:6: error: [SDC012] clock 'zero' has period 0"
    "bad.sdc:10: warning: [SDC013] generated clock 'gcopy': no clock reaches its -source rst_n"
    "bad.sdc:11: warning: [SDC013] generated clock 'gbad': -edges takes three edges, it has 2"
    "bad.sdc:7: warning: [SDC014] virtual clock 'vfree' is used by no"
    "soc.sv:49: warning: [SDC015] 1 register bit is clocked by 'clk_x', which no clock reaches"
    "[SDC018] 6 clocks (clk_a, clk_b2, scan_clk, div2, gcopy, gbad) have no set_clock_uncertainty"
    "soc.sv:22: warning: [SDC021] output port 'x_out' has no set_output_delay"
    "bad.sdc:18: warning: [SDC023] -min delay 3 on 'en' is above its -max delay 1"
    "bad.sdc:21: warning: [SDC024] set_input_delay on 'dout', which is an output port"
    "bad.sdc:22: warning: [SDC024] set_input_delay on 'clk_b', which is a clock source"
    "bad.sdc:25: warning: [SDC024] set_output_delay on 'en', which is an input port"
    "bad.sdc:20: warning: [SDC025] set_input_delay has no -clock"
    "bad.sdc:24: warning: [SDC027] set_output_delay on 'flag_b' replaces the one at"
    "bad.sdc:28: warning: [SDC030] set_false_path: a -from, -through or -to names nothing"
    "bad.sdc:38: warning: [SDC030] set_false_path has no -from, -through or -to"
    "bad.sdc:29: warning: [SDC031] set_false_path -from dout[7]: this is an output port"
    "bad.sdc:30: warning: [SDC031] set_false_path -from u_core/flag_reg/D: this is the data input of a register"
    "bad.sdc:29: warning: [SDC032] set_false_path -to din[7]: this is an input port"
    "bad.sdc:30: warning: [SDC032] set_false_path -to u_core/acc_reg[0]/Q: this is the output of a register"
    "bad.sdc:31: warning: [SDC033] set_false_path: the design has no path between these points"
    "bad.sdc:32: warning: [SDC034] set_multicycle_path 3 -setup has no matching -hold"
    "bad.sdc:34: warning: [SDC035] set_max_delay is on the same paths as the set_false_path at"
    "bad.sdc:35: warning: [SDC035] set_max_delay replaces the one at"
    "bad.sdc:33: warning: [SDC037] set_multicycle_path 0: a setup multiplier below 1"
    "bad.sdc:36: warning: [SDC037] set_min_delay 9 is above the set_max_delay 6"
    "bad.sdc:15: warning: [SDC041] clocks 'clk_a' and 'div2' are declared asynchronous, but one derives from the other"
    "bad.sdc:13: warning: [SDC042] set_clock_groups: clock 'clk_a' is in two groups"
    "bad.sdc:14: error: [SDC042] set_clock_groups needs one of"
    "bad.sdc:41: warning: [SDC050] set_case_analysis 1 on test_mode contradicts the value 0"
    "bad.sdc:42: error: [SDC050] set_case_analysis value '2'"
    "[SDC060] 5 input ports have no set_driving_cell"
    "[SDC061] 4 output ports have no set_load")
  string(FIND "${bad}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "bad.sdc, missing: ${want}\n${bad}")
  endif()
endforeach()
if(NOT bad MATCHES "msdc: 49 findings")
  message(FATAL_ERROR "bad.sdc: expected 49 findings:\n${bad}")
endif()

run(mux.sdc 0)
foreach(want
    "clk_b registers: 5"
    "data pins: 10"
    "soc.sv:45: warning: [SDC016] clocks clk_b, scan_clk all reach 'mclk' (1 register bit)"
    "soc.sv:21: warning: [SDC020] input port 'b_in' has no set_input_delay"
    "mux.sdc:6: warning: [SDC040] clock 'clk_b' launches data that clock 'scan_clk' captures (1 path, such as raw_b_reg to muxed_reg)"
    "msdc: 5 findings")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "mux.sdc, missing: ${want}\n${out}")
  endif()
endforeach()

# The same report on one thread and on many.
run(bad.sdc 1 -threads 1)
if(NOT out STREQUAL bad)
  message(FATAL_ERROR "-threads 1 changed the report:\n${out}")
endif()
run(bad.sdc 1 -threads 7)
if(NOT out STREQUAL bad)
  message(FATAL_ERROR "-threads 7 changed the report:\n${out}")
endif()

# -disable, -Werror, -quiet, -json
run(mux.sdc 1 -Werror -disable SDC060 -disable SDC061 -quiet -json ${OUT}/msdc.json)
if(out MATCHES "SDC06" OR out MATCHES "findings")
  message(FATAL_ERROR "-disable or -quiet did not take:\n${out}")
endif()
file(READ ${OUT}/msdc.json js)
if(NOT js MATCHES "\"rule\": \"SDC040\"" OR NOT js MATCHES "\"name\": \"div2\", \"period\": 0, \"generated\": true")
  message(FATAL_ERROR "msdc.json:\n${js}")
endif()

# With no design the file is checked on its own.
execute_process(COMMAND ${MSDC} -sdc ${SRC}/tests/sdc/bad.sdc RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 1 OR NOT out MATCHES "SDC010" OR NOT out MATCHES "SDC034" OR out MATCHES "SDC003|SDC015|SDC033")
  message(FATAL_ERROR "msdc with no design (rc ${rc}):\n${out}")
endif()
