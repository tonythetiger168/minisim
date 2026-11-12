# The xrun / irun front end (src/xrun.inc, docs/XRUN.md): msim run as xrun
# (a link), as irun, and as msim -xrun, with -f / -F files, -incdir, -define,
# -top, -timescale, ignored options, the log file, -input Tcl, -elaborate,
# and VHDL refused.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_xrun.cmake
set(D ${SRC}/tests/xrun)
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
file(CREATE_LINK ${MSIM} ${OUT}/xrun SYMBOLIC)
file(CREATE_LINK ${MSIM} ${OUT}/irun SYMBOLIC)

function(run want_rc)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY ${D}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL want_rc)
    message(FATAL_ERROR "${ARGN} exited ${rc}, not ${want_rc}: ${err}\n${out}")
  endif()
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
endfunction()
function(expect text re)
  if(NOT text MATCHES "${re}")
    message(FATAL_ERROR "no match for '${re}' in\n${text}")
  endif()
endfunction()

# As xrun, with the log named: the run's output goes to stdout and the log.
run(0 ${OUT}/xrun -64bit -sv -access +rwc -f run.f -top worklib.tb:sv -timescale 1ns/1ps +stop
    -licqueue -l ${OUT}/my.log)
expect("${out}" "^hi W=8 seed-ok\nn=10 at 100000\n$")
file(READ ${OUT}/my.log log)
expect("${log}" "^hi W=8 seed-ok\nn=10 at 100000\n$")

# As irun with +nc options: the default log is irun.log in the working directory.
run(0 ${OUT}/irun -f run.f +nctop+tb +access+rw +stop)
expect("${out}" "n=10 at 100\n")
expect("${err}" "^$")
file(READ ${D}/irun.log log)
file(REMOVE ${D}/irun.log)
expect("${log}" "n=10 at 100\n")

# msim -xrun with an -input Tcl script: run with units, value, exit; -nolog.
run(0 ${MSIM} -xrun -f run.f -input run.tcl -nolog -linedebug)
file(REMOVE ${D}/xrun.do)
expect("${out}" "time 50: [^\n]*\ntb.n = 32'h5\ntime 70: [^\n]*\nquit at time 70\n")
if(EXISTS ${D}/xrun.log)
  message(FATAL_ERROR "-nolog wrote xrun.log")
endif()

# -elaborate stops before the run.
run(0 ${MSIM} -xrun -f run.f -elaborate -nolog)
expect("${out}" "not run")
if(out MATCHES "seed-ok")
  message(FATAL_ERROR "-elaborate ran the design:\n${out}")
endif()

# VHDL is refused with a message, not passed on.
run(2 ${MSIM} -xrun tb.sv x.vhd -nolog)
expect("${err}" "VHDL is not supported")

# The same front end with --compat=cadence (compat.hpp), anywhere on the line,
# and Cadence options in any case: -SV, -ACCESS +rwc and -NOCOPYRIGHT are
# dropped, -TIMESCALE is renamed.
run(0 ${MSIM} -f run.f -SV --compat=cadence -ACCESS +rwc -TIMESCALE 1ns/1ps -NOCOPYRIGHT -nolog +stop)
expect("${out}" "n=10 at 100000\n")
