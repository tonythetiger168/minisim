# mlint self-test: the rules fire on tests/lint/lint.sv and tests/lint/cdc.sv,
# and nothing fires on tests/lint/clean.sv.
#   cmake -DMLINT=<mlint> -DSRC=<source dir> -P mlint.cmake
function(run file args)
  execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/${file} ${args}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mlint ${file} exited ${rc}\n${out}${err}")
  endif()
endfunction()

run(lint.sv "-top;t")
foreach(want
    "[L001] 't.y' is not assigned on every path"
    "[L002] blocking assignment to 't.r' in an always_ff"
    "[L003] non-blocking assignment to 't.z' in an always_comb"
    "[L004] 't.z' is assigned by 2 processes")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "missing: ${want}\n${out}")
  endif()
endforeach()
if(NOT out MATCHES "4 findings")
  message(FATAL_ERROR "expected 4 findings on lint.sv:\n${out}")
endif()

# -disable turns one rule off.
run(lint.sv "-top;t;-disable;L001;-disable;L004")
if(NOT out MATCHES "2 findings" OR out MATCHES "L001|L004")
  message(FATAL_ERROR "-disable did not take:\n${out}")
endif()

# CDC: one flop is not a synchronizer, and a wide bus needs more than flops.
run(cdc.sv "-top;t")
if(NOT out MATCHES "\\[CDC001\\] 't.rq'.*through one flop only")
  message(FATAL_ERROR "cdc.sv: the single-flop crossing was not reported:\n${out}")
endif()
if(NOT out MATCHES "\\[CDC002\\] 't.dreg'.*8 bits wide")
  message(FATAL_ERROR "cdc.sv: the wide crossing was not reported:\n${out}")
endif()
# The two-flop synchronizer and the signal through the sub-module are clean.
if(out MATCHES "t.s1|t.s2|t.b1")
  message(FATAL_ERROR "cdc.sv: a clean crossing was reported:\n${out}")
endif()

# Nothing to report, and -Werror still exits 0.
run(clean.sv "-top;clean;-Werror")
if(NOT out MATCHES "0 findings")
  message(FATAL_ERROR "clean.sv is not clean:\n${out}")
endif()

# -Werror exits 1 when there is a finding.
execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/lint.sv -top t -Werror -quiet
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 1 OR out MATCHES "findings")
  message(FATAL_ERROR "-Werror -quiet: rc=${rc}\n${out}")
endif()
message(STATUS "mlint: ok")
