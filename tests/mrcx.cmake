# mrcx self-test on tests/rcx: the SPEF of a small routed design, the options,
# and the same file for any thread count.
#   cmake -DMRCX=<mrcx> -DSRC=<source dir> -DOUT=<scratch dir> -P mrcx.cmake
file(MAKE_DIRECTORY ${OUT})
set(D ${SRC}/tests/rcx)

function(run expect)
  execute_process(COMMAND ${MRCX} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
  if(NOT rc EQUAL ${expect})
    message(FATAL_ERROR "mrcx ${ARGN}: exit ${rc}, expected ${expect}\n${out}${err}")
  endif()
endfunction()
function(need text what)
  foreach(want ${ARGN})
    string(FIND "${text}" "${want}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "${what}: missing '${want}' in\n${text}")
    endif()
  endforeach()
endfunction()

# The process file's stack.
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -itf ${D}/demo.itf -o ${OUT}/demo.spef -report ${OUT}/demo.rpt
    -date fixed -threads 1)
need("${out}" "summary" "demo: 5 nets, 24.9 um of wire" "4 coupling capacitors" "demo2m stack, 25 C")
if(out MATCHES "open nets|shorts")
  message(FATAL_ERROR "the demo design is clean:\n${out}")
endif()
file(READ ${OUT}/demo.spef spef)
need("${spef}" "demo.spef"
     "*SPEF \"IEEE 1481-2009\"" "*DESIGN \"demo\"" "*C_UNIT 1 FF" "*R_UNIT 1 OHM"
     "*NAME_MAP\n*1 in\n*2 n1\n*3 n2\n*4 in2\n*5 out[0]\n*6 u1"
     "*PORTS\n*1 I\n*4 I\n*5 O"
     "*P *1 I *C 0.0000 2.1000"
     "*I *6:Y O *C 1.5000 2.1000 *D INV"
     "*2:3 *2:4 30.4\n"          # 7.6 um of metal1: 0.4 ohm/sq x 7.6 / 0.1
     "*3:3 *3:4 6\n"             # one via1 cut
     "*3:6 *3:4 5.4\n")          # 1.8 um of metal2: 0.3 x 1.8 / 0.1
# n1 and n2 run side by side: the coupling is in both nets.
string(REGEX MATCHALL "\\*2:3 \\*3:3 0\\.229[0-9]*" a "${spef}")
string(REGEX MATCHALL "\\*3:3 \\*2:3 0\\.229[0-9]*" b "${spef}")
if(NOT a OR NOT b)
  message(FATAL_ERROR "coupling between n1 and n2 is not in both nets:\n${spef}")
endif()
file(READ ${OUT}/demo.rpt rpt)
need("${rpt}" "report" "# net  length_um" "n1  7.800  31.2  " "out[0]  3.700  18.6  ")

# Any thread count writes the same file.
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -itf ${D}/demo.itf -o ${OUT}/demo4.spef -date fixed -threads 4 -q)
file(READ ${OUT}/demo4.spef spef4)
if(NOT spef STREQUAL spef4)
  message(FATAL_ERROR "-threads 4 and -threads 1 differ")
endif()
if(NOT out STREQUAL "")
  message(FATAL_ERROR "-q still printed:\n${out}")
endif()

# Hotter metal: 0.4 x (1 + 0.003 x 100) = 0.52 ohm/sq, so 30.4 becomes 39.52.
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -itf ${D}/demo.itf -o ${OUT}/hot.spef -temp 125 -no_name_map)
need("${out}" "hot summary" "125 C")
file(READ ${OUT}/hot.spef hot)
need("${hot}" "hot.spef" "n1:3 n1:4 39.52\n" "*I u1:Y O" "*D_NET out[0] ")
if(hot MATCHES "NAME_MAP")
  message(FATAL_ERROR "-no_name_map still wrote a name map")
endif()

# Decoupled: no capacitor names two nodes, the totals stay.
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -itf ${D}/demo.itf -o ${OUT}/gnd.spef -no_coupling)
need("${out}" "decoupled summary" "0 coupling capacitors")
file(READ ${OUT}/gnd.spef gnd)
if(gnd MATCHES "\n[0-9]+ \\*[0-9:]+ \\*[0-9:]+ [0-9.e-]+\n\\*RES" OR gnd MATCHES "\\*2:3 \\*3:3")
  message(FATAL_ERROR "-no_coupling left a coupling capacitor")
endif()

# Without a process file the LEF's own values are used.
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -o ${OUT}/lef.spef)
need("${out}" "LEF summary" "lef stack")

# Corners: one read of the design, a file per corner, and each file is what a
# run with only that corner's options writes.
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -o ${OUT}/mc.spef -report ${OUT}/mc.rpt -date fixed
    -corner lef -corner hot -itf ${D}/demo.itf -temp 125 -corner cold -itf ${D}/demo.itf -temp -40 -cscale 0.9)
need("${out}" "corners" "corner hot" "demo2m stack, 125 C" "corner cold" "demo2m stack, -40 C" "corner lef" "lef stack, 25 C"
     "mc.hot.spef" "mc.cold.spef" "mc.lef.spef")
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -itf ${D}/demo.itf -o ${OUT}/hot.spef -date fixed -temp 125 -q)
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -itf ${D}/demo.itf -o ${OUT}/cold.spef -date fixed -temp -40 -cscale 0.9 -q)
run(0 -lef ${D}/demo.lef -def ${D}/demo.def -o ${OUT}/lef1.spef -date fixed -q)
foreach(c hot cold)
  file(READ ${OUT}/mc.${c}.spef many)
  file(READ ${OUT}/${c}.spef one)
  if(NOT many STREQUAL one)
    message(FATAL_ERROR "corner ${c} differs from a run of its own")
  endif()
endforeach()
file(READ ${OUT}/mc.lef.spef many)
file(READ ${OUT}/lef1.spef one)
if(NOT many STREQUAL one)
  message(FATAL_ERROR "corner lef differs from a run of its own")
endif()
file(READ ${OUT}/mc.cold.rpt rpt)
need("${rpt}" "corner report" "corner cold, demo2m stack, -40 C")
run(2 -lef ${D}/demo.lef -def ${D}/demo.def -corner a -corner a)
run(2 -lef ${D}/demo.lef -def ${D}/demo.def -o ${OUT}/x.spef -corner a -corner b -itf ${D}/no_such.itf)
if(EXISTS ${OUT}/x.a.spef)
  message(FATAL_ERROR "a bad process file in the second corner must stop the run before the first is written")
endif()

# Opens and shorts are reported, and -Werror turns them into exit 1.
run(0 -lef ${D}/demo.lef -def ${D}/broken.def -o ${OUT}/broken.spef)
need("${err}" "broken" "open: net gap is in 2 parts" "short: nets s1 and s2 overlap on metal1")
need("${out}" "broken summary" "1 open nets, 0 unrouted nets, 1 shorts")
run(1 -lef ${D}/demo.lef -def ${D}/broken.def -o ${OUT}/broken.spef -Werror -q)

# Errors.
run(2 -lef ${D}/demo.lef -def ${D}/no_such.def)
run(2 -lef ${D}/demo.lef -def ${D}/demo.def -bogus)
run(2)
run(0 -version)
need("${out}" "version" "mrcx ")
