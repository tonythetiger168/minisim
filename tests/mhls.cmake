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
  set(src ${H}/${file}.c)
  set(inc)
  if(file MATCHES "\\.cpp$")   # C++ goes through clang
    set(src ${H}/${file} -clang -I${SRC}/include)
    set(inc -I${SRC}/include)
  endif()
  set(cref -cref ${base}_ref.cpp)
  if(NOT nocref EQUAL -1)
    set(cref)
  endif()
  execute_process(COMMAND ${MHLS} ${src} -top ${top} -o ${base}.sv -tb ${base}_tb.sv ${cref}
                          -vectors 40 -q ${opts}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mhls ${file}.c -top ${top} ${shown} failed:\n${out}${err}")
  endif()
  if(cref)
    execute_process(COMMAND ${CXX} -std=c++17 -fwrapv -w ${inc} -o ${base}_ref ${base}_ref.cpp
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
design(looppipe2 hist)                           # one iteration every 2 cycles: a memory updated in place
design(looppipe2 walk)                           # pointer chase
design(looppipe2 copyz -clock 2.5)               # the loop ends on a value read from memory
design(faint faint)                              # variables that only feed themselves are removed
design(mac mac -handshake)                       # ready/valid ports around the FSM: the result waits for out_ready
design(fir fir -handshake -pipeline -clock 2.5)  # around the pipeline: a refused result stalls every stage
design(looppipe2 hist -handshake)                # with memories
design(stream scale)                             # stream arguments: one in, one out, a stalling source and sink
design(stream packet)                            # reads that depend on data read before, a conditional write
design(stream merge)                             # two sources, reads in branches
design(stream relay)                             # streams handed to an inlined function
design(stream relay -unroll-all -handshake)
design(stream peak)                              # pipelined loop over streams: one element per cycle, stalls as a whole
design(stream scale -pipeline-loops)
design(stream relay -pipeline-loops -clock 3)    # one iteration every 2 cycles, two reads per iteration
design(fir fir -pipeline-loops -clock 3)         # best effort: loops that cannot be pipelined stay as they are
design(matmul matmul -pipeline-loops -clock 3)
design(sqrt hist -pipeline-loops)
design(structs track -pipeline-loops -clock 3)
design(apint blend NOCREF)
design(apint blend -pipeline -clock 2 NOCREF)

# What cannot be built says why.
function(refuses file top pattern)
  set(src ${H}/${file}.c)
  if(file MATCHES "\\.cpp$")
    set(src ${H}/${file} -clang)
  endif()
  execute_process(COMMAND ${MHLS} ${src} -top ${top} -o ${OUT}/refused.sv -q ${ARGN}
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
refuses(bad_stream_both echo "cannot also be written")
refuses(bad_stream_empty peek "'empty' is not supported")
refuses(bad_stream_local local "a stream is a reference parameter")
refuses(bad_struct_byvalue byvalue "pass 'x' by pointer or reference")
refuses(bad_struct_dropped dropped "must be assigned to a variable")
refuses(bad_struct_scalar scalar "a struct cannot be used as a value")
refuses(gcd nosuch "is not defined")

# Delays from a Liberty library: the slow corner needs more states than the fast one.
design(fir fir -lib ${SRC}/lib/mini45/mini45_tt.lib -clock 2)
design(arith arith -lib ${SRC}/lib/mini45/mini45_ss.lib -clock 1.5 -mul 1 -div 1)
design(looppipe sum -lib ${SRC}/lib/mini45/mini45_ff.lib -clock 2)
function(states corner var)
  execute_process(COMMAND ${MHLS} ${H}/fir.c -top fir -lib ${SRC}/lib/mini45/mini45_${corner}.lib -clock 2 -o ${OUT}/fir_${corner}.sv
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0 OR NOT "${out}${err}" MATCHES "delays from mini45_${corner}: inverter" OR NOT "${out}${err}" MATCHES ", ([0-9]+) states")
    message(FATAL_ERROR "mhls -lib mini45_${corner}.lib:\n${out}${err}")
  endif()
  set(${var} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()
states(ss slow)
states(ff fast)
if(NOT slow GREATER fast)
  message(FATAL_ERROR "mhls -lib: fir takes ${slow} states at the slow corner and ${fast} at the fast one; the slow corner should need more")
endif()
message(STATUS "mhls -lib: fir at 2 ns takes ${slow} states with mini45_ss, ${fast} with mini45_ff")
refuses(gcd gcd "mhls: .*gcd.c" -lib ${H}/gcd.c)

# Two-port memories.
design(dualport pairs -dual-port)                # two reads per state
design(dualport hist -dual-port)                 # updated in place at one iteration per cycle (forwarding)
design(dualport hist -dual-port -handshake)
design(dualport bump -dual-port)                 # a write that does not always happen
design(dualport bump -dual-port -clock 1.6)      # every third cycle
design(dualport copyfwd -dual-port)
design(dualport runmax -dual-port)               # a local memory
design(dualport swp -dual-port)
design(looppipe2 hist -dual-port)
design(looppipe2 walk -dual-port)
design(matmul matmul -dual-port)
design(structs track -dual-port)
refuses(dualport stencil "accessed more than 2 times per iteration" -dual-port)
refuses(dualport copyfwd "a memory has one port")
# Pipelined functions with memories.
design(pipemem lookup -pipeline)
design(pipemem lookup -pipeline -clock 2)
design(pipemem logw -pipeline)
design(pipemem count -pipeline -dual-port)
design(pipemem count -pipeline -dual-port -handshake)
design(pipemem delay -pipeline -dual-port)
design(pipemem xfer -pipeline -dual-port -clock 2)
design(pipemem count4 -pipeline -dual-port)
refuses(pipemem count "accessed more than once per call, and a memory has one port" -pipeline)

# Pipelined functions with streams.
design(pipestream scale -pipeline)
design(pipestream scale -pipeline -handshake)
design(pipestream filt -pipeline)                # a write that does not always happen
design(pipestream filt -pipeline -handshake)
design(pipestream gen -pipeline)
design(pipestream sum -pipeline -clock 4)        # a stream and a memory
refuses(pipestream twice "stream 'in' is read or written more than once per call" -pipeline)
execute_process(COMMAND ${MHLS} ${H}/dualport.c -top hist -dual-port -o ${OUT}/hist_rate.sv
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT "${out}${err}" MATCHES "pipelined, 3 stages, one iteration per cycle")
  message(FATAL_ERROR "mhls -dual-port: the histogram loop should run at one iteration per cycle:\n${out}${err}")
endif()

# C++ through clang (mhls -clang): classes, templates, constructors, operators.
find_program(CLANG NAMES clang)
if(CLANG)
  set(ENV{MHLS_CLANG} ${CLANG})
  design(cxx.cpp fir4)                           # class templates, constructors, operators, a scoped enum
  design(cxx.cpp fir4 -unroll-all -clock 4 -handshake)
  design(cxx.cpp gather)                         # an object fed from a stream
  design(cxx.cpp gather -pipeline-loops)
  design(cxx2.cpp filt)                          # nested class, static and template members, overloads, default arguments
  design(cxx2.cpp filt -unroll-all -clock 5)
  design(cxx2.cpp tally)                         # member functions that return *this: +=, ++, chained calls
  design(cxx2.cpp tally -unroll-all)
  design(cxx3.cpp survey)                        # inheritance: base constructors, inherited and hidden members, two bases
  design(cxx3.cpp survey -unroll-all -clock 6)
  design(cxx3.cpp shade)                         # a derived struct as an argument
  design(cxxhdr.cpp ring)                        # classes and templates from a header, std::min and std::max
  refuses(bad_cxx_base.cpp top "virtual functions in a class with a base")
  refuses(bad_cxx_float.cpp top "does not support floating point")
  refuses(bad_cxx_lambda.cpp top "lambda")
  refuses(bad_cxx_syntax.cpp top "clang did not accept")
  if(PY)
    set(sim)
    if(MSIM)
      set(sim --msim ${MSIM})
    endif()
    # The fuzzer's C programs are C++ too: the same checks with clang reading them.
    execute_process(COMMAND ${PY} ${H}/fuzz.py --mhls ${MHLS} ${sim} --cxx ${CXX} --out ${OUT}/fuzz_clang --seeds 0:40 --clang
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "mhls fuzz through clang:\n${out}${err}")
    endif()
    string(STRIP "${out}" out)
    message(STATUS "through clang: ${out}")
  endif()
else()
  message(STATUS "mhls -clang: no clang on this machine, C++ designs skipped")
endif()

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
  execute_process(COMMAND ${PY} ${H}/fuzz.py --mhls ${MHLS} ${sim} --cxx ${CXX} --out ${OUT}/fuzz_dp --seeds 0:80 --dual-port --pipe-io
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mhls fuzz with two-port memories:\n${out}${err}")
  endif()
  string(STRIP "${out}" out)
  message(STATUS "two-port memories: ${out}")
endif()
