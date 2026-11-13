# The waveform tasks of Verdi (FSDB), VCS (VPD) and Xcelium (SHM) compile on
# msim and write FST under the same name with .fst: the scopes they name,
# memories with $fsdbDumpMDA / $vcdplusmemon, off / on, and a note that the
# vendor format itself is not written. mdebug reads the source and the FST.
#   cmake -DMSIM=<msim> -DMDEBUG=<mdebug> -DCHK=<test_fst> -DSRC=<source dir> -DOUT=<dir> -P msim_vendor_dump.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
function(sim name sv fst)
  execute_process(COMMAND ${MSIM} ${SRC}/tests/debug/${sv} WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${name}: msim exited ${rc}\n${err}")
  endif()
  if(NOT err MATCHES "the vendors' own formats; writing ${fst} \\(open it with mdebug")
    message(FATAL_ERROR "${name}: no note about the FST:\n${err}")
  endif()
  if(err MATCHES "error|ignored")
    message(FATAL_ERROR "${name}: unexpected messages:\n${err}")
  endif()
  if(NOT EXISTS ${OUT}/${fst})
    message(FATAL_ERROR "${name}: ${fst} not written")
  endif()
  execute_process(COMMAND ${CHK} ${OUT}/${fst} RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "test_fst ${fst} failed: ${err}")
  endif()
  set(wave "\n${o}" PARENT_SCOPE)
endfunction()
function(has name text)
  foreach(want ${ARGN})
    if(NOT text MATCHES "${want}")
      message(FATAL_ERROR "${name}: expected '${want}' in\n${text}")
    endif()
  endforeach()
endfunction()
function(lacks name text)
  foreach(bad ${ARGN})
    if(text MATCHES "${bad}")
      message(FATAL_ERROR "${name}: did not expect '${bad}' in\n${text}")
    endif()
  endforeach()
endfunction()

# FSDB: everything under tb, memories, nothing between $fsdbDumpoff (20) and $fsdbDumpon (40).
sim(fsdb vendor_fsdb.sv wave.fst)
has(fsdb "${wave}" "\n0 tb.c 0000\n" "\n0 tb.mem\\[0\\] 00010001\n" "\n15 tb.u1.q 0010\n" "\n40 tb.c 0100\n" "\n45 tb.u1.q 0101\n")
lacks(fsdb "${wave}" "\n2[05] " "\n3[05] ")

# VPD: only tb.u1 and the memory named by $vcdplusmemon; $vcdpluson resumes after $vcdplusoff.
sim(vpd vendor_vpd.sv run.fst)
has(vpd "${wave}" "\n0 tb.u1.q 0000\n" "\n0 tb.mem\\[3\\] xxxxxxxx\n" "\n40 tb.u1.q 0100\n")
lacks(vpd "${wave}" " tb.clk " " tb.q " "\n2[05] " "\n3[05] ")

# SHM: $shm_probe(tb, "AS") is tb's own level.
sim(shm vendor_shm.sv waves.fst)
has(shm "${wave}" "\n0 tb.c 0000\n" "\n25 tb.c 0011\n")

# mdebug compiles the same source and reads the FST.
execute_process(COMMAND ${MDEBUG} ${SRC}/tests/debug/vendor_fsdb.sv -vcd ${OUT}/wave.fst -active tb.c -at 45
                RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mdebug exited ${rc}\n${err}\n${o}")
endif()
has(mdebug "${o}" "tb.c = 4'h5 @45\n  <- [^\n]*vendor_fsdb.sv:10: c <= c \\+ 1")

# The same from msim -i / -do: Verdi's UCLI commands fsdbDumpfile, fsdbDumpvars, fsdbDumpoff.
file(WRITE ${OUT}/ucli.do "fsdbDumpfile ui.fsdb\nfsdbDumpvars 1 tb\nrun 12\nfsdbDumpoff\nrun\n")
execute_process(COMMAND ${MSIM} ${SRC}/tests/debug/vendor_shm.sv -do ${OUT}/ucli.do WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT o MATCHES "dumping to ui.fst from the next time slot")
  message(FATAL_ERROR "ucli: msim exited ${rc}\n${err}\n${o}")
endif()
execute_process(COMMAND ${CHK} ${OUT}/ui.fst OUTPUT_VARIABLE wave)
has(ucli "\n${wave}" "\n0 tb.c 0000\n" "\n5 tb.c 0001\n")
lacks(ucli "\n${wave}" "\n15 ")
