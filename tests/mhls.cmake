# mhls end to end. Every design is synthesized, then the same random calls
# are checked (1) with the host C++ compiler against mhls's reference run and
# (2) in msim on the generated RTL, when msim was built (-DMSIM=...).
#   cmake -DMHLS=... [-DMSIM=...] -DCXX=... -DSRC=<repo> -DOUT=<dir> [-DPY=python3] -P tests/mhls.cmake
file(MAKE_DIRECTORY ${OUT})
set(H ${SRC}/tests/hls)

function(design file top)
  set(opts ${ARGN})
  list(FIND opts NOCREF nocref)
  list(REMOVE_ITEM opts NOCREF)
  string(REPLACE ";" " " shown "${opts}")
  set(base ${OUT}/${top})
  set(cref -cref ${base}_ref.cpp)
  if(NOT nocref EQUAL -1)
    set(cref)
  endif()
  execute_process(COMMAND ${MHLS} ${H}/${file}.c -top ${top} -o ${base}.sv -tb ${base}_tb.sv ${cref}
                          -vectors 40 -q ${opts}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mhls ${file}.c -top ${top} ${shown} failed:\n${out}${err}")
  endif()
  if(cref)
    execute_process(COMMAND ${CXX} -std=c++17 -fwrapv -w -o ${base}_ref ${base}_ref.cpp
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "${top} ${shown}: the C reference does not compile:\n${out}${err}")
    endif()
    execute_process(COMMAND ${base}_ref RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "PASS")
      message(FATAL_ERROR "${top} ${shown}: mhls disagrees with the host compiler:\n${out}${err}")
    endif()
  endif()
  if(MSIM)
    execute_process(COMMAND ${MSIM} ${base}.sv ${base}_tb.sv -top ${top}_tb
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 300)
    if(NOT out MATCHES "PASS: ${top}")
      message(FATAL_ERROR "${top} ${shown}: the RTL disagrees with the reference in msim:\n${out}${err}")
    endif()
    string(STRIP "${out}" out)
    message(STATUS "${file} ${shown}: ${out}")
  endif()
endfunction()

design(gcd gcd)
design(arith arith)
design(arith arith -clock 1.2 -mul 1 -div 1)     # multi-cycle dividers, shared units
design(arith arith -pipeline)
design(fir fir)
design(fir fir -pipeline -clock 2.5)             # static delay line in a 2-stage pipeline
design(fir fir -clock 3 -mul 2)
design(mem mem)
design(mem mem -clock 2)
design(ctrl ctrl)
design(ctrl ctrl -clock 1.5)
design(sqrt hist)                                # a 256-entry local array becomes a RAM
design(sqrt isqrt)
design(mac mac -mul 1)
design(mac mac -mul 1 -no-share)
design(mac mac -pipeline)
design(calls calls)
design(calls calls -clock 2 -mul 1)
design(matmul matmul)
design(matmul matmul -unroll-all -mul 2 -clock 4)
design(alu alu)                                  # switch as multiplexers, fall-through, break in a loop
design(alu alu -clock 2 -unroll-all)
design(structs cmac)                             # structs by value, struct returned
design(structs grade)                            # struct by pointer, nested members, bit-fields
design(structs track)                            # array of structs, struct with an array member
design(structs track -clock 20 -max-reg-array 2)
design(structs rot)                              # structs through a pipeline
design(looppipe sum)                             # pipelined loops: a memory read per iteration
design(looppipe scale)                           # read one memory, write another, carried maximum
design(looppipe scale -clock 2.5)                # more stages
design(looppipe mix)                             # pipelined inner loop, entered once per outer iteration
design(fir fir -pipeline-loops -clock 3)         # best effort: loops that cannot be pipelined stay as they are
design(matmul matmul -pipeline-loops -clock 3)
design(sqrt hist -pipeline-loops)
design(structs track -pipeline-loops -clock 3)
design(apint blend NOCREF)
design(apint blend -pipeline -clock 2 NOCREF)

# What cannot be built says why.
function(refuses file top pattern)
  execute_process(COMMAND ${MHLS} ${H}/${file}.c -top ${top} -o ${OUT}/refused.sv -q ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0 OR NOT err MATCHES "${pattern}")
    message(FATAL_ERROR "mhls ${file}.c -top ${top} ${ARGN}: expected an error matching '${pattern}', got rc=${rc}:\n${err}")
  endif()
endfunction()
refuses(gcd gcd "cannot be pipelined" -pipeline)
refuses(arith arith "takes more than one clock period" -pipeline -clock 3)
refuses(mac mac "static 'total' is read in stage" -pipeline -clock 4)
refuses(bad_recursion fact "recursion")
refuses(bad_looppipe_port bump "accessed more than once per iteration")
refuses(bad_looppipe_cond length "condition is not known in the first stage")
refuses(bad_looppipe_chain chase "updated from its own previous value")
refuses(bad_struct_byvalue byvalue "pass 'x' by pointer or reference")
refuses(bad_struct_dropped dropped "must be assigned to a variable")
refuses(bad_struct_scalar scalar "a struct cannot be used as a value")
refuses(gcd nosuch "is not defined")

# Threads: the same files whatever -threads says (two tops at once, and the clock sweep).
foreach(n 1 4)
  execute_process(COMMAND ${MHLS} ${H}/sqrt.c -top hist,isqrt -o ${OUT}/thr${n} -threads ${n} -report ${OUT}/thr${n}/report.json
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out${n} ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mhls -threads ${n} failed:\n${err}")
  endif()
  execute_process(COMMAND ${MHLS} ${H}/arith.c -top arith -explore 1.5,2,3,5,10,20 -threads ${n}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE sweep${n} ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mhls -explore -threads ${n} failed:\n${err}")
  endif()
endforeach()
foreach(f hist.sv isqrt.sv report.json)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/thr1/${f} ${OUT}/thr4/${f} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${f} differs between -threads 1 and -threads 4")
  endif()
endforeach()
if(NOT sweep1 STREQUAL sweep4)
  message(FATAL_ERROR "-explore differs between -threads 1 and -threads 4:\n${sweep1}\n${sweep4}")
endif()
if(NOT sweep1 MATCHES "clock ns")
  message(FATAL_ERROR "-explore printed no table:\n${sweep1}")
endif()

# Random programs, the same three-way check.
if(PY)
  set(sim)
  if(MSIM)
    set(sim --msim ${MSIM})
  endif()
  execute_process(COMMAND ${PY} ${H}/fuzz.py --mhls ${MHLS} ${sim} --cxx ${CXX} --out ${OUT}/fuzz --seeds 0:150
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mhls fuzz:\n${out}${err}")
  endif()
  string(STRIP "${out}" out)
  message(STATUS "${out}")
endif()
