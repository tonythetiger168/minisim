# The multi-step front ends (src/mstep.inc, docs/COMPAT.md): Siemens vlib,
# vmap, vlog, vcom, vsim and Synopsys vlogan, vhdlan, vcs, simv, run as
# links to msim. Each flow analyses a package into a library of its own, a
# VHDL entity and an SV testbench into work, then simulates the testbench.
# A file with a syntax error is not added; analysing a file again replaces
# its line.
#   cmake -DMSIM=<msim> -DSRC=<source dir> -DOUT=<dir> -P msim_mstep.cmake
set(D ${SRC}/tests/mstep)
set(VHD ${SRC}/tests/vhdl/counter.vhd)
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT}/bin ${OUT}/q ${OUT}/s ${OUT}/one)
foreach(t vlib vmap vlog vcom vsim vlogan vhdlan vcs)
  file(CREATE_LINK ${MSIM} ${OUT}/bin/${t} SYMBOLIC)
endforeach()
set(B ${OUT}/bin)

function(run dir want_rc)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY ${dir}
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
function(lines_of file re want)
  file(STRINGS ${file} l REGEX "${re}")
  list(LENGTH l n)
  if(NOT n EQUAL want)
    message(FATAL_ERROR "${file}: ${n} lines match '${re}', not ${want}")
  endif()
endfunction()

# (A ; would split the CMake argument: the -do commands are on lines.)
# ---- Siemens: vlib, vmap, vlog, vcom, vsim ----
set(Q ${OUT}/q)
run(${Q} 2 ${B}/vlog ${D}/pkg.sv)            # no library yet
expect("${err}" "vlib work")
run(${Q} 0 ${B}/vlib work)
run(${Q} 0 ${B}/vlib libs/pkgs)
run(${Q} 0 ${B}/vmap pkgs libs/pkgs)
expect("${out}" "Modifying modelsim.ini")
run(${Q} 0 ${B}/vmap pkgs)
expect("${out}" "pkgs maps to directory libs/pkgs")
run(${Q} 0 ${B}/vlog -work pkgs -sv ${D}/pkg.sv)
run(${Q} 0 ${B}/vcom -2008 ${VHD})
run(${Q} 0 ${B}/vlog -sv -quiet +incdir+${D}/inc +define+W=8 ${D}/tb.sv -l comp.log)
run(${Q} 0 ${B}/vlog -sv +incdir+${D}/inc +define+W=8 ${D}/tb.sv)   # again: one line
lines_of(${Q}/work/msim.lib "tb[.]sv" 1)
lines_of(${Q}/work/msim.lib "counter[.]vhd" 1)
lines_of(${Q}/libs/pkgs/msim.lib "pkg[.]sv" 1)
run(${Q} 1 ${B}/vlog ${D}/bad.sv)
expect("${out}${err}" "not added to library work")
lines_of(${Q}/work/msim.lib "bad[.]sv" 0)

run(${Q} 0 ${B}/vsim -c -L pkgs -do "run -all\nquit -f" work.tb +more -l run.log)
expect("${out}" "hi W=8 count=3\nmore\n")
file(READ ${Q}/run.log log)
expect("${log}" "hi W=8 count=3\nmore\n")
# A -do script with more than run and quit goes to msim's Tcl.
run(${Q} 0 ${B}/vsim -c -L pkgs -sv_seed 5 -do "puts start\nrun -all\nquit -f" tb)
expect("${out}" "start\n.*hi W=8 count=3\n")
# A package library left out: the design does not elaborate.
run(${Q} 1 ${B}/vsim -c -do "run -all" tb)

# ---- Synopsys: vlogan, vhdlan, vcs, simv ----
set(S ${OUT}/s)
file(WRITE ${S}/synopsys_sim.setup "-- libraries\nWORK > DEFAULT\nDEFAULT : ./work\nPKGS : ./pkgs\n")
run(${S} 0 ${B}/vlogan -full64 -sverilog -work PKGS ${D}/pkg.sv)
run(${S} 0 ${B}/vhdlan ${VHD})
run(${S} 0 ${B}/vlogan -sverilog -timescale=1ns/1ps +incdir+${D}/inc +define+W=8 ${D}/tb.sv)
lines_of(${S}/pkgs/msim.lib "pkg[.]sv" 1)
lines_of(${S}/work/msim.lib "tb[.]sv" 1)
run(${S} 0 ${B}/vcs -full64 -debug_access+all -kdb tb -o simv -l comp.log)
if(NOT EXISTS ${S}/simv)
  message(FATAL_ERROR "vcs wrote no simv")
endif()
run(${S} 0 ${S}/simv +more -l run.log)
expect("${out}" "hi W=8 count=3\nmore\n")
file(READ ${S}/run.log log)
expect("${log}" "hi W=8 count=3\nmore\n")

# ---- vcs in one step, with -R ----
set(O ${OUT}/one)
run(${O} 0 ${B}/vcs -sverilog +incdir+${D}/inc +define+W=8 ${D}/pkg.sv ${VHD} ${D}/tb.sv -o sim1 -R +more)
expect("${out}" "hi W=8 count=3\nmore\n")
run(${O} 0 ${O}/sim1)
expect("${out}" "hi W=8 count=3\n")
# An error at vcs time: no simv.
run(${O} 1 ${B}/vcs -sverilog ${D}/bad.sv -o sim2)
if(EXISTS ${O}/sim2)
  message(FATAL_ERROR "vcs wrote sim2 for a design with errors")
endif()

# The same front end as msim -vlog / -vsim.
run(${Q} 0 ${MSIM} -vlog -sv +incdir+${D}/inc +define+W=8 ${D}/tb.sv)
run(${Q} 0 ${MSIM} -vsim -c -L pkgs -do "run -all\nquit -f" tb)
expect("${out}" "hi W=8 count=3\n")
