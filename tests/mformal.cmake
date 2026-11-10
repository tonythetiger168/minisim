# mformal self-test: the verdict of every property of tests/formal/*.sv, the
# same output for any thread count, and each counterexample replayed in msim.
#   cmake -DMFORMAL=<mformal> -DMSIM=<msim> -DSRC=<source dir> -DOUT=<work dir> -P mformal.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
function(run file wantrc args)
  execute_process(COMMAND ${MFORMAL} ${SRC}/tests/formal/${file} -o ${OUT}/${file} ${args}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  if(NOT rc EQUAL ${wantrc})
    message(FATAL_ERROR "mformal ${file} exited ${rc}, expected ${wantrc}\n${out}${err}")
  endif()
endfunction()
function(expect file)
  foreach(want ${ARGN})
    if(NOT out MATCHES "${want}")
      message(FATAL_ERROR "${file}: missing '${want}'\n${out}")
    endif()
  endforeach()
endfunction()
# The counterexample of `prop`, replayed in msim: the assertion fails at `time`.
function(replay file prop time)
  execute_process(COMMAND ${MSIM} ${SRC}/tests/formal/${file} ${OUT}/${file}/${prop}_tb.sv -top mformal_replay
                          --relax-enum-conversions -Wno-unconnected-output-port
                  OUTPUT_VARIABLE o ERROR_VARIABLE e)
  if(NOT "${o}${e}" MATCHES "${prop}: at time ${time}000")
    message(FATAL_ERROR "${file}: msim does not fail ${prop} at ${time}\n${o}${e}")
  endif()
endfunction()

run(counter.sv 1 "-top;counter;-tb")
expect(counter.sv
  "assert  counter.a_range +proven"
  "assert  counter.a_wrap +proven"
  "assert  counter.a_step +proven"
  "assert  counter.a_lt5 +falsified  at cycle 5"
  "cover   counter.c_nine +covered  at cycle 9"
  "cover   counter.c_seq +covered  at cycle 5"
  "cover   counter.c_twelve +unreachable"
  "7 properties: 3 proven, 1 falsified, 2 covered, 1 unreachable")
replay(counter.sv counter.a_lt5 65)
if(NOT EXISTS ${OUT}/counter.sv/counter.a_lt5.vcd OR NOT EXISTS ${OUT}/counter.sv/counter.c_nine.vcd)
  message(FATAL_ERROR "counter.sv: the traces were not written")
endif()
file(READ ${OUT}/counter.sv/counter.a_lt5.vcd vcd)
if(NOT vcd MATCHES "\\$scope module counter" OR NOT vcd MATCHES "\\$var wire 4 [^ ]+ cnt \\[3:0\\]" OR NOT vcd MATCHES "b0101 ")
  message(FATAL_ERROR "counter.sv: the counterexample VCD does not show cnt reaching 5\n${vcd}")
endif()

# The same lines whatever the thread count.
set(ref "")
foreach(t 1 2 7)
  run(fsm.sv 1 "-top;fsm;-quiet;-threads;${t}")
  if(ref STREQUAL "")
    set(ref "${out}")
  elseif(NOT out STREQUAL ref)
    message(FATAL_ERROR "fsm.sv: -threads ${t} changes the result\n${ref}\n---\n${out}")
  endif()
endforeach()
run(fsm.sv 1 "-top;fsm;-tb;-json;${OUT}/fsm.json")
expect(fsm.sv
  "assert  fsm.a_bounded +proven"
  "assert  fsm.a_bounded_bad +falsified  at cycle 4"
  "assert  fsm.a_fin +proven"
  "assert  fsm.a_gnt_once +proven"
  "assert  fsm.a_gnt_run +proven"
  "assert  fsm.a_imm +proven"
  "assert  fsm.a_imm_bad +falsified  at cycle 2"
  "assert  fsm.a_live +proven"
  "assert  fsm.a_no_skip +proven"
  "assert  fsm.a_until +proven"
  "cover   fsm.c_fin +covered  at cycle 3"
  "assume  fsm.m_done +assumed")
replay(fsm.sv fsm.a_bounded_bad 55)
file(READ ${OUT}/fsm.json js)
if(NOT js MATCHES "\"name\": \"fsm.a_bounded_bad\", \"kind\": \"assert\", \"result\": \"falsified\", \"cycle\": 4")
  message(FATAL_ERROR "fsm.sv: -json\n${js}")
endif()

# A hierarchy with a memory; a_push_fills needs the other proofs as lemmas.
run(fifo.sv 1 "-top;top;-tb")
expect(fifo.sv
  "assert  top.a_top +proven"
  "assert  top.u.a_count +proven"
  "assert  top.u.a_full_stays +proven"
  "assert  top.u.a_not_both +proven"
  "assert  top.u.a_push_fills +proven  \\(induction with lemmas"
  "assert  top.u.a_push_counts +falsified  at cycle 5"
  "cover   top.u.c_full +covered  at cycle 4"
  "cover   top.u.c_full_then_empty +covered  at cycle 8")
replay(fifo.sv top.u.a_push_counts 65)
# -prop picks one; -bmc proves nothing.
run(fifo.sv 0 "-top;top;-prop;a_count;-bmc;-depth;6")
expect(fifo.sv "assert  top.u.a_count +bounded  no counterexample in 6 cycles" "1 property: 1 bounded")

# Sequence operators, assumptions, no reset.
run(seq.sv 1 "-top;seq;-tb")
expect(seq.sv
  "no reset"
  "assert  seq.a_goto +proven"
  "assert  seq.a_iff +proven"
  "assert  seq.a_nexttime +proven"
  "assert  seq.a_once +proven"
  "assert  seq.a_rep +proven"
  "assert  seq.a_thr +proven"
  "assert  seq.a_tight +falsified  at cycle 3"
  "assert  seq.a_window +proven"
  "cover   seq.c_fast +covered  at cycle 2"
  "cover   seq.c_never +unreachable"
  "cover   seq.c_seq +covered"
  "cover   seq.c_slow +covered  at cycle 5")
replay(seq.sv seq.a_tight 35)

# What cannot be modelled is cut and listed; the proof that does not need it holds.
run(cut.sv 1 "-top;cut;-v")
expect(cut.sv
  "assert  cut.a_q +proven"
  "assert  cut.a_slow +falsified"
  "assert  cut.a_n +falsified"
  "n: a process on another clock or edge"
  "slow: a process that is not combinational or clocked")

# Proofs that need an invariant (PDR), and a failure deeper than -depth.
run(inv.sv 1 "-top;inv;-depth;10;-tb")
expect(inv.sv
  "assert  inv.a_acc +proven  \\(pdr"
  "assert  inv.a_c +proven"
  "assert  inv.a_ring +proven"
  "assert  inv.a_same +proven"
  "assert  inv.a_deep +falsified  at cycle 35")
replay(inv.sv inv.a_deep 365)
run(inv.sv 0 "-top;inv;-depth;10;-no-pdr;-prop;a_deep;-prop;a_acc")
expect(inv.sv "assert  inv.a_deep +bounded  no counterexample in 10 cycles" "assert  inv.a_acc +bounded")

# Liveness: proofs, and counterexamples that are loops.
run(live.sv 1 "-top;live")
expect(live.sv
  "assert  live.a_prio0 +proven"
  "assert  live.a_prio1 +falsified  at cycle [0-9]+ .*liveness: from here the design can repeat cycles"
  "assert  live.a_rr0 +proven"
  "assert  live.a_rr1 +proven"
  "assert  live.a_rr1_fast +falsified"
  "assert  live.a_until +proven"
  "assert  live.a_always_ev +proven"
  "assert  live.a_idle +falsified  at cycle [0-9]+ .*liveness")
