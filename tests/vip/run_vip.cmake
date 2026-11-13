# Run one test of a VIP self-test bench (tests/vip/<name>/files.f) on msim.
#   cmake -DMSIM=<msim> -DUVM_HOME=<uvm-core> -DROOT=<repo> -DF=<files.f>
#         -DTOP=<top> -DTEST=<uvm test> -P run_vip.cmake
# Pass: msim exits 0, the UVM summary shows no UVM_ERROR and no UVM_FATAL,
# and the bench prints "SB_PASS checks=<n>" (its scoreboard made checks and
# none failed).
execute_process(COMMAND ${MSIM} -uvmhome ${UVM_HOME} -f ${F} -top ${TOP} +UVM_TESTNAME=${TEST}
                OUTPUT_VARIABLE got ERROR_VARIABLE err RESULT_VARIABLE rc TIMEOUT 900
                WORKING_DIRECTORY ${ROOT})
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "msim exited ${rc} on ${F} ${TEST}:\n${err}\n${got}")
endif()
foreach(need "UVM_ERROR :    0" "UVM_FATAL :    0" "SB_PASS checks=")
  string(FIND "${got}" "${need}" at)
  if(at EQUAL -1)
    string(LENGTH "${got}" n)
    if(n GREATER 6000)
      math(EXPR from "${n} - 6000")
      string(SUBSTRING "${got}" ${from} -1 got)
    endif()
    message(FATAL_ERROR "${F} ${TEST}: no \"${need}\" in the output:\n...${got}")
  endif()
endforeach()
