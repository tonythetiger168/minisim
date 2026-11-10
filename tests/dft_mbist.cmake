# mdft -mbist self-test: generate controllers for several memories and
# algorithms and run each generated testbench with msim (and Icarus when it is
# installed). Every testbench instance holds a RAM with one injected fault; it
# passes only if the hardware flags exactly the faults, at exactly the address,
# that mdft's fault simulator predicted.
#   cmake -DMDFT=<mdft> -DMSIM=<msim> [-DIVERILOG=<iverilog> -DVVP=<vvp>] -DOUT=<scratch dir> -P dft_mbist.cmake
file(MAKE_DIRECTORY ${OUT})

function(want_in text)
  foreach(w ${ARGN})
    string(FIND "${text}" "${w}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "missing: ${w}\n${text}")
    endif()
  endforeach()
endfunction()

set(n 0)
foreach(cfg
    "-words;16;-width;8"
    "-words;13;-width;5;-latency;2;-backgrounds;all"
    "-words;8;-width;1;-latency;0;-algo;mats+"
    "-words;32;-width;16;-algo;marchss;-latency;3;-backgrounds;2"
    "-words;1;-width;4"
    "-words;20;-width;4;-algo;marchlr"
    "-words;2;-width;1;-latency;2;-algo;marchb"
    "-words;24;-width;64;-backgrounds;all;-algo;marcha"
    "-words;10;-width;3;-algo;marchx;-latency;0"
    "-words;9;-width;6;-march;any(w1)\;down(r1,w0,r0)\;up(r0,w1)\;any(r1)")
  math(EXPR n "${n} + 1")
  execute_process(COMMAND ${MDFT} -mbist -name ram${n} ${cfg} -o ${OUT}/ram${n}.v -tb ${OUT}/ram${n}_tb.v -quiet
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mdft -mbist ${cfg} exited ${rc}\n${out}${err}")
  endif()
  execute_process(COMMAND ${MSIM} ${OUT}/ram${n}.v ${OUT}/ram${n}_tb.v -top ram${n}_bist_tb
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT out MATCHES "MBIST TB PASS")
    message(FATAL_ERROR "msim, mdft -mbist ${cfg}:\n${out}${err}")
  endif()
  if(IVERILOG AND VVP)
    execute_process(COMMAND ${IVERILOG} -g2005 -o ${OUT}/ram${n}.vvp -s ram${n}_bist_tb ${OUT}/ram${n}.v ${OUT}/ram${n}_tb.v
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "iverilog, mdft -mbist ${cfg}:\n${out}${err}")
    endif()
    execute_process(COMMAND ${VVP} ${OUT}/ram${n}.vvp OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT out MATCHES "MBIST TB PASS")
      message(FATAL_ERROR "Icarus, mdft -mbist ${cfg}:\n${out}${err}")
    endif()
  endif()
endforeach()

# The fault simulator agrees with the textbook: March C- finds every stuck-at,
# transition, address and between-word coupling fault; MATS+ only stuck-at and
# address faults; all backgrounds close the within-word gap.
execute_process(COMMAND ${MDFT} -mbist -name m -words 64 -width 8 -o ${OUT}/m.v
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
want_in("${out}"
  "m_bist: 64 x 8, any(w0); up(r0,w1); up(r1,w0); down(r0,w1); down(r1,w0); any(r0)"
  "10N operations, 1 background: 642 clocks"
  "stuck-at                                 64 / 64     100.0%"
  "transition                               64 / 64     100.0%"
  "inversion coupling, between words      1792 / 1792   100.0%"
  "idempotent coupling, between words     3584 / 3584   100.0%"
  "idempotent coupling, within a word      192 / 384     50.0%"
  "state coupling, between words          3584 / 3584   100.0%"
  "address decoder                          56 / 56     100.0%")
execute_process(COMMAND ${MDFT} -mbist -name m -words 64 -width 8 -backgrounds all -o ${OUT}/m.v -threads 1
                OUTPUT_VARIABLE one)
execute_process(COMMAND ${MDFT} -mbist -name m -words 64 -width 8 -backgrounds all -o ${OUT}/m.v -threads 8
                OUTPUT_VARIABLE many)
want_in("${one}" "total                                 10104 / 10104  100.0%")
if(NOT one STREQUAL many)
  message(FATAL_ERROR "-threads 1 and -threads 8 report different coverage:\n${one}\n${many}")
endif()
execute_process(COMMAND ${MDFT} -mbist -name m -words 64 -width 8 -algo mats+ -o ${OUT}/m.v OUTPUT_VARIABLE out)
want_in("${out}"
  "stuck-at                                 64 / 64     100.0%"
  "transition                               32 / 64      50.0%"
  "address decoder                          56 / 56     100.0%")

foreach(bad "-words;0;-width;8" "-words;8" "-words;8;-width;8;-algo;nope" "-words;8;-width;8;-march;up(x1)"
            "-words;8;-width;4;-backgrounds;9")
  execute_process(COMMAND ${MDFT} -mbist ${bad} -o ${OUT}/bad.v
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR NOT err MATCHES "^mdft: ")
    message(FATAL_ERROR "mdft -mbist ${bad} should fail: rc=${rc}\n${out}${err}")
  endif()
endforeach()
message(STATUS "mdft -mbist: ok")
