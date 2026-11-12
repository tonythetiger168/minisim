# Class objects in the waveform (msim -vcd-class [N]): tests/debug/objwave.sv
# moves a handle tb.p through two objects and null. Each property of the
# object tb.p points to is a signal tb.p.addr that follows the handle (x while
# it is null), base class properties first, strings left out; with N = 2 the
# handle property tb.p.next is followed one level more. mdebug reads the
# views back (value, changes, -signals). The waveform is the same for any
# thread count.
#   cmake -DMSIM=<msim> -DMDEBUG=<mdebug> -DSRC=<source dir> -DOUT=<dir> -P msim_vcd_class.cmake
set(SV ${SRC}/tests/debug/objwave.sv)
file(MAKE_DIRECTORY ${OUT})

function(run name)
  execute_process(COMMAND ${MSIM} ${SV} ${ARGN} WORKING_DIRECTORY ${OUT}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "msim ${ARGN} exited ${rc}: ${err}\n${out}")
  endif()
  file(READ ${OUT}/${name} vcd)
  set(vcd "${vcd}" PARENT_SCOPE)
endfunction()

# Header: the views sit in a scope named after the handle.
run(c1.vcd -vcd-class -vcd c1.vcd)
foreach(want
    "\\$scope module p \\$end\n\\$var reg 4 [^ ]+ kind \\$end\n\\$var reg 8 [^ ]+ addr \\$end\n\\$var reg 16 [^ ]+ data \\$end\n\\$var reg 64 [^ ]+ next \\$end\n\\$upscope \\$end")
  if(NOT vcd MATCHES "${want}")
    message(FATAL_ERROR "-vcd-class: no match for '${want}' in\n${vcd}")
  endif()
endforeach()
if(vcd MATCHES " tag ")
  message(FATAL_ERROR "-vcd-class: the string property is in the waveform\n${vcd}")
endif()
if(vcd MATCHES "scope module next")
  message(FATAL_ERROR "-vcd-class: tb.p.next is followed at depth 1\n${vcd}")
endif()

# Values, through mdebug.
execute_process(COMMAND ${CMAKE_COMMAND} -E echo_append
"value tb.p.addr 7
value tb.p.addr 12
value p.data 22
value tb.p.data 27
value tb.p.kind 6
changes tb.p.addr
value tb.p.nope 1
quit
" OUTPUT_FILE ${OUT}/c1.script)
execute_process(COMMAND ${MDEBUG} ${SV} -vcd ${OUT}/c1.vcd -script ${OUT}/c1.script
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
foreach(want
    "\"signal\":\"tb.p.addr\",\"time\":7,\"value\":\"8'h11\""
    "\"signal\":\"tb.p.addr\",\"time\":12,\"value\":\"8'h22\""
    "\"signal\":\"tb.p.data\",\"time\":22,\"value\":\"16'h1234\""
    "\"signal\":\"tb.p.data\",\"time\":27,\"value\":\"16'bxxxxxxxxxxxxxxxx\""
    "\"signal\":\"tb.p.kind\",\"time\":6,\"value\":\"4'h3\""
    "\"changes\":\\[\\[0,\"xxxxxxxx\"\\],\\[5,\"00010001\"\\],\\[10,\"00100010\"\\],\\[20,\"00110011\"\\],\\[25,\"xxxxxxxx\"\\]\\]"
    "\"ok\":false,\"error\":\"no signal 'tb.p.nope' in the design\"")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "mdebug on -vcd-class: no match for '${want}' in\n${out}\n${err}")
  endif()
endforeach()
execute_process(COMMAND ${MDEBUG} ${SV} -vcd ${OUT}/c1.vcd -signals OUTPUT_VARIABLE out)
if(NOT out MATCHES "tb.p.addr  property of the object tb.p points to  \\(8 bits, from the waveform\\)")
  message(FATAL_ERROR "mdebug -signals: no object view in\n${out}")
endif()

# Depth 2: tb.p.next.addr is the address of the next object.
run(c2.vcd -vcd-class 2 -vcd c2.vcd)
if(NOT vcd MATCHES "\\$scope module next \\$end\n\\$var reg 4 [^ ]+ kind \\$end\n\\$var reg 8 [^ ]+ addr \\$end")
  message(FATAL_ERROR "-vcd-class 2: no tb.p.next scope in\n${vcd}")
endif()
file(WRITE ${OUT}/c2.script "value tb.p.next.addr 17\nvalue tb.p.next.addr 22\n")
execute_process(COMMAND ${MDEBUG} ${SV} -vcd ${OUT}/c2.vcd -script ${OUT}/c2.script OUTPUT_VARIABLE out)
foreach(want "\"time\":17,\"value\":\"8'h33\"" "\"time\":22,\"value\":\"8'bxxxxxxxx\"")
  if(NOT out MATCHES "${want}")
    message(FATAL_ERROR "-vcd-class 2: no match for '${want}' in\n${out}")
  endif()
endforeach()

# Same waveform on 1 and 4 threads; none of it without -vcd-class.
run(t1.vcd -vcd-class 2 -threads 1 -vcd t1.vcd)
set(one "${vcd}")
run(t4.vcd -vcd-class 2 -threads 4 -vcd t4.vcd)
if(NOT one STREQUAL vcd)
  message(FATAL_ERROR "-vcd-class: the waveform depends on the thread count")
endif()
run(plain.vcd -vcd plain.vcd)
if(vcd MATCHES "scope module p ")
  message(FATAL_ERROR "object views without -vcd-class\n${vcd}")
endif()
