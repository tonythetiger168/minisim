# mdft self-test: every rule fires on tests/dft/dft.sv, nothing fires on
# tests/dft/clean.sv, and the chain plan counts the flops per domain.
#   cmake -DMDFT=<mdft> -DSRC=<source dir> -P mdft.cmake
function(run file args)
  execute_process(COMMAND ${MDFT} ${SRC}/tests/dft/${file} ${args}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mdft ${file} exited ${rc}\n${out}${err}")
  endif()
endfunction()

run(dft.sv "-top;t")
foreach(want
    "[DFT001] 't.gclk' is made by combinational logic"
    "[DFT002] the asynchronous reset 't.soft_rst' is driven by logic"
    "[DFT003] an always_latch is not scannable"
    "[DFT004] this block uses the falling edge of 't.clk'"
    "[DFT005] combinational loop"
    "[DFT006] 't.divclk' is driven by a flop")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing: ${want}\n${out}")
  endif()
endforeach()
if(NOT out MATCHES "6 findings")
  message(FATAL_ERROR "expected 6 findings on dft.sv:\n${out}")
endif()
# The chain plan: the flops per domain, and both edges of the one clock.
if(NOT out MATCHES "5 flops in 3 clock domains" OR
   NOT out MATCHES "t\\.clk: 3 flops \\(2 rising, 1 falling\\)")
  message(FATAL_ERROR "chain plan:\n${out}")
endif()

run(dft.sv "-top;t;-disable;DFT001;-disable;DFT005")
if(NOT out MATCHES "4 findings" OR out MATCHES "DFT001|DFT005")
  message(FATAL_ERROR "-disable did not take:\n${out}")
endif()

run(clean.sv "-top;clean;-Werror")
if(NOT out MATCHES "0 findings" OR NOT out MATCHES "2 flops in 1 clock domain")
  message(FATAL_ERROR "clean.sv is not scan-ready:\n${out}")
endif()

execute_process(COMMAND ${MDFT} ${SRC}/tests/dft/dft.sv -top t -Werror -quiet
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 1 OR out MATCHES "findings|flops in")
  message(FATAL_ERROR "-Werror -quiet: rc=${rc}\n${out}")
endif()
message(STATUS "mdft: ok")
