# mlint self-test: the first rules fire on tests/lint/lint.sv and
# tests/lint/cdc.sv, and nothing fires on tests/lint/clean.sv. The rest of the
# rules, the configuration and the waivers are in tests/lint/check_rules.py.
#   cmake -DMLINT=<mlint> -DSRC=<source dir> -P mlint.cmake
function(run file args)
  execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/${file} ${args}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mlint ${file} exited ${rc}\n${out}${err}")
  endif()
endfunction()

run(lint.sv "-top;t;-only;L001,L002,L003,L004")
foreach(want
    "[L001] 'y' is not assigned on every path"
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
run(lint.sv "-top;t;-only;L001,L002,L003,L004;-disable;L001;-disable;L004")
if(NOT out MATCHES "2 findings" OR out MATCHES "L001|L004")
  message(FATAL_ERROR "-disable did not take:\n${out}")
endif()

# CDC: one flop is not a synchronizer, and a wide bus needs more than flops.
run(cdc.sv "-top;t;-only;cdc")
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

# Every crossing rule fires once on cdc_bad.sv (CDC003 names both sources).
run(cdc_bad.sv "-top;cdc_bad;-only;cdc,RDC001,RDC002,RDC003")
foreach(want
    "[CDC001] 'cdc_bad.a1' (clocked by cdc_bad.aclk) is read by logic clocked by cdc_bad.bclk with no synchronizer"
    "[CDC002] 'cdc_bad.abus' (clocked by cdc_bad.aclk) is read by logic clocked by cdc_bad.bclk through 2 flops, but it is 4 bits wide"
    "[CDC003] 'cdc_bad.a1' (clocked by cdc_bad.aclk) is read by logic clocked by cdc_bad.bclk through combinational logic in front of the synchronizer 'cdc_bad.g1'"
    "[CDC003] 'cdc_bad.a2'"
    "[CDC004] 'cdc_bad.a3', 'cdc_bad.a4' (clocked by cdc_bad.aclk) are synchronized separately and meet again at 'cdc_bad.o_recon'"
    "[CDC005] 'cdc_bad.a3' (clocked by cdc_bad.aclk) is synchronized 2 times into the domain of cdc_bad.bclk ('cdc_bad.s3', 'cdc_bad.f1')"
    "[RDC002] asynchronous reset 'cdc_bad.rst_n' is used directly by flops in 2 clock domains (cdc_bad.aclk, cdc_bad.bclk)"
    "7 findings (CDC001 1, CDC002 1, CDC003 2, CDC004 1, CDC005 1, RDC002 1)")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_bad.sv missing: ${want}\n${out}")
  endif()
endforeach()

# The safe schemes: a shift-register synchronizer in a module, an
# enable-qualified bus, a Gray pointer, reset synchronizers, a gated clock.
# The divided clock is a domain of its own until -clock-group says otherwise.
run(cdc_clean.sv "-top;cdc_clean;-only;cdc,RDC001,RDC002,RDC003")
if(NOT out MATCHES "\\[CDC001\\] 'cdc_clean.req' \\(clocked by cdc_clean.aclk\\) is read by logic clocked by cdc_clean.div"
   OR NOT out MATCHES "\\[RDC001\\] asynchronous reset 'cdc_clean.a_rst_n' \\(clocked by cdc_clean.aclk\\) resets 1 flop clocked by cdc_clean.div"
   OR NOT out MATCHES "2 findings")
  message(FATAL_ERROR "cdc_clean.sv: expected the divided clock's two findings only:\n${out}")
endif()
run(cdc_clean.sv "-top;cdc_clean;-only;cdc,RDC001,RDC002,RDC003;-clock-group;aclk,div;-Werror;-cdc-report")
foreach(want
    "clock domains: 2"
    "cdc_clean.req (cdc_clean.aclk) -> cdc_clean.u_req.s (cdc_clean.bclk)  4-flop synchronizer"
    "cdc_clean.hold (cdc_clean.aclk) -> cdc_clean.dout (cdc_clean.bclk)  enable-qualified"
    "cdc_clean.gray (cdc_clean.bclk) -> cdc_clean.gsync (cdc_clean.aclk)  2-flop synchronizer, Gray coded"
    "mlint: 0 findings")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_clean.sv -cdc-report missing: ${want}\n${out}")
  endif()
endforeach()
# -static: a1 is said not to change while it is read, so its two findings go.
run(cdc_bad.sv "-top;cdc_bad;-only;cdc,RDC001,RDC002,RDC003;-static;cdc_bad.a1")
if(out MATCHES "cdc_bad.a1'" OR NOT out MATCHES "5 findings \\(CDC002 1, CDC003 1, CDC004 1, CDC005 1, RDC002 1\\)")
  message(FATAL_ERROR "cdc_bad.sv -static did not take:\n${out}")
endif()

# Bits of one bus are separate clocks, and a bus synchronized bit by bit is
# one Gray-coded crossing, not four synchronizers of one signal.
run(cdc_bits.sv "-top;cdc_bits;-only;cdc,RDC001,RDC002,RDC003;-cdc-report")
foreach(want
    "clock domains: 2"
    "cdc_bits.clk[0]  1 flop"
    "cdc_bits.clk[1]  4 flops"
    "cdc_bits.u.w (cdc_bits.clk[0]) -> cdc_bits.g[3].i_s.r (cdc_bits.clk[1])  2-flop synchronizer, Gray coded"
    "mlint: 0 findings")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_bits.sv missing: ${want}\n${out}")
  endif()
endforeach()

# Reset crossings; RDC004 is off until -enable.
run(rdc.sv "-top;rdc;-only;cdc,RDC001,RDC002,RDC003")
foreach(want
    "[RDC001] asynchronous reset 'rdc.wd_rst_n' (clocked by rdc.aclk) resets 1 flop clocked by rdc.bclk"
    "[RDC002] asynchronous reset 'rdc.por_n' is used directly by flops in 2 clock domains (rdc.aclk, rdc.bclk)"
    "[RDC003] 'rdc.s', reset by 'rdc.soft_n', feeds 'rdc.q3', reset by 'rdc.por_n', in the domain of rdc.aclk"
    "3 findings")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "rdc.sv missing: ${want}\n${out}")
  endif()
endforeach()
run(rdc.sv "-top;rdc;-only;cdc,RDC001,RDC002,RDC003;-enable;RDC004")
if(NOT out MATCHES "\\[RDC004\\] 'rdc.s', reset by 'rdc.soft_n', feeds 'rdc.q4', which has no asynchronous reset" OR NOT out MATCHES "4 findings")
  message(FATAL_ERROR "rdc.sv -enable RDC004:\n${out}")
endif()

# -sdc: the file says which clocks are related. Clocks alone: primary clocks
# are asynchronous, a generated clock goes with its master, and a clock the
# file does not define is CDC006.
run(cdc_sdc.sv "-top;cdc_sdc;-only;cdc,RDC001,RDC002,RDC003;-sdc;${SRC}/tests/lint/cdc_sdc_none.sdc;-cdc-report")
foreach(want
    "cdc_sdc.div2  1 flop  SDC clock DIV"
    "cdc_sdc.mclk  1 flop  SDC clock C, T"
    "cdc_sdc.uclk  1 flop  no SDC clock"
    "[CDC001] 'cdc_sdc.a0' (clocked by cdc_sdc.aclk) is read by logic clocked by cdc_sdc.bclk"
    "[CDC001] 'cdc_sdc.d0' (clocked by cdc_sdc.div2) is read by logic clocked by cdc_sdc.bclk"
    "[CDC001] 'cdc_sdc.b0' (clocked by cdc_sdc.bclk) is read by logic clocked by cdc_sdc.cclk"
    "[CDC001] 'cdc_sdc.c0' (clocked by cdc_sdc.cclk) is read by logic clocked by cdc_sdc.mclk"
    "[CDC006] clock 'cdc_sdc.uclk' of 1 flop is not a clock in the SDC file"
    "6 findings (CDC001 4, CDC006 1, CDC008 1)")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_sdc.sv -sdc cdc_sdc_none.sdc missing: ${want}\n${out}")
  endif()
endforeach()
# set_clock_groups: A against B only; exclusive clocks are no crossing, and
# set_case_analysis makes the clock mux select constant (no CDC008).
run(cdc_sdc.sv "-top;cdc_sdc;-only;cdc,RDC001,RDC002,RDC003;-sdc;${SRC}/tests/lint/cdc_sdc_groups.sdc")
if(NOT out MATCHES "'cdc_sdc.a0' \\(clocked by cdc_sdc.aclk\\) is read by logic clocked by cdc_sdc.bclk"
   OR NOT out MATCHES "'cdc_sdc.d0' \\(clocked by cdc_sdc.div2\\) is read by logic clocked by cdc_sdc.bclk"
   OR NOT out MATCHES "2 findings \\(CDC001 2\\)")
  message(FATAL_ERROR "cdc_sdc.sv -sdc cdc_sdc_groups.sdc:\n${out}")
endif()
# The same constraints written with variables, loops, bare names, wildcards,
# -hierarchical and commands that change no domain give the same result.
set(groups_out "${out}")
run(cdc_sdc.sv "-top;cdc_sdc;-only;cdc,RDC001,RDC002,RDC003;-sdc;${SRC}/tests/lint/cdc_sdc_syntax.sdc")
if(NOT out STREQUAL groups_out)
  message(FATAL_ERROR "cdc_sdc.sv -sdc cdc_sdc_syntax.sdc differs from cdc_sdc_groups.sdc:\n${out}")
endif()
# A false path between two clocks makes that pair asynchronous.
run(cdc_sdc.sv "-top;cdc_sdc;-only;cdc,RDC001,RDC002,RDC003;-sdc;${SRC}/tests/lint/cdc_sdc_false.sdc")
if(NOT out MATCHES "'cdc_sdc.b0' \\(clocked by cdc_sdc.bclk\\) is read by logic clocked by cdc_sdc.cclk"
   OR NOT out MATCHES "2 findings \\(CDC001 1, CDC008 1\\)")
  message(FATAL_ERROR "cdc_sdc.sv -sdc cdc_sdc_false.sdc:\n${out}")
endif()

# CDC004 through flops: where the signals meet, once, to the depth asked for.
run(cdc_seq.sv "-top;cdc_seq;-only;cdc")
foreach(want
    "[CDC004] 'cdc_seq.a1', 'cdc_seq.a2' (clocked by cdc_seq.aclk) are synchronized separately and meet again at 'cdc_seq.o1', 1 flop after the synchronizers (and at 1 more flop)"
    "[CDC004] 'cdc_seq.a1', 'cdc_seq.a3' (clocked by cdc_seq.aclk) are synchronized separately and meet again at 'cdc_seq.o3', 2 flops after the synchronizers:"
    "2 findings (CDC004 2)")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_seq.sv missing: ${want}\n${out}")
  endif()
endforeach()
run(cdc_seq.sv "-top;cdc_seq;-only;cdc;-cdc-depth;3")
if(NOT out MATCHES "meet again at 'cdc_seq.o4', 3 flops after the synchronizers" OR NOT out MATCHES "3 findings")
  message(FATAL_ERROR "cdc_seq.sv -cdc-depth 3:\n${out}")
endif()
run(cdc_seq.sv "-top;cdc_seq;-only;cdc;-cdc-depth;0")
if(NOT out MATCHES "0 findings")
  message(FATAL_ERROR "cdc_seq.sv -cdc-depth 0:\n${out}")
endif()

# The reset tree: one-flop reset synchronizers and logic on a reset; a
# two-flop synchronizer behind a scan mux is clean and in the report.
run(rdc_tree.sv "-top;rdc_tree;-only;cdc,rdc;-cdc-report")
foreach(want
    "reset synchronizers: 4"
    "rdc_tree.rst_n (rdc_tree.clk)  2 flops, resets 1 flop, from rdc_tree.por_n"
    "rdc_tree.e2 (rdc_tree.clk)  2 flops, resets 1 flop"
    "[RDC005] reset synchronizer 'rdc_tree.one_n' (clocked by rdc_tree.clk) is one flop"
    "[RDC005] reset synchronizer 'rdc_tree.ext_q' (clocked by rdc_tree.clk) is one flop"
    "[RDC006] asynchronous reset 'rdc_tree.both_n' of 1 flop is combinational logic of 2 flops ('rdc_tree.sw', 'rdc_tree.wd')"
    "3 findings (RDC005 2, RDC006 1)")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "rdc_tree.sv missing: ${want}\n${out}")
  endif()
endforeach()

# Crossing waivers: by source and reading clock, by file, and a reset.
run(cdc_bad.sv "-top;cdc_bad;-only;cdc,RDC001,RDC002,RDC003;-waivers;${SRC}/tests/lint/cdc_waivers.cfg;-show-waived")
foreach(want
    "waived: [CDC001] 'cdc_bad.a1' (clocked by cdc_bad.aclk)"
    "waived: [CDC005] 'cdc_bad.a3'"
    "waived: [RDC002] asynchronous reset 'cdc_bad.rst_n'"
    "4 findings (CDC002 1, CDC003 2, CDC004 1), 3 waived")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_bad.sv with waivers missing: ${want}\n${out}")
  endif()
endforeach()

# Bits of a vector and fields of a struct written separately are separate
# signals: two synchronizers in one vector are clean, a first stage read by
# logic is found by its bit.
run(cdc_slices.sv "-top;cdc_slices;-only;cdc,RDC001,RDC002,RDC003;-cdc-report")
foreach(want
    "cdc_slices.a1 (cdc_slices.aclk) -> cdc_slices.s[0] (cdc_slices.bclk)  3-flop synchronizer"
    "cdc_slices.a2 (cdc_slices.aclk) -> cdc_slices.s[2] (cdc_slices.bclk)  3-flop synchronizer"
    "cdc_slices.c_a.req (cdc_slices.aclk) -> cdc_slices.q1 (cdc_slices.bclk)  2-flop synchronizer"
    "cdc_slices.a1 (cdc_slices.aclk) -> cdc_slices.r[1] (cdc_slices.bclk)  one flop only (CDC001)"
    "1 finding (CDC001 1)")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_slices.sv missing: ${want}\n${out}")
  endif()
endforeach()

# Block models: -cdc-abstract writes cdc_block's model (the golden one in
# tests/lint/cdc_block.cdc), and the SoC gives the same findings with the RTL,
# with the model in place of the RTL, and with the block as a black box.
execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/cdc_block.sv -top cdc_block -only cdc
                        -cdc-abstract cdc_block_model.cdc
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ cdc_block_model.cdc got)
file(READ ${SRC}/tests/lint/cdc_block.cdc want)
if(NOT rc EQUAL 0 OR NOT got STREQUAL want)
  message(FATAL_ERROR "-cdc-abstract on cdc_block.sv (exit ${rc}):\n${got}\nexpected:\n${want}\n${err}")
endif()
foreach(variant "cdc_block.sv" "cdc_block.sv;-cdc-model;${SRC}/tests/lint/cdc_block.cdc"
                "cdc_block_bb.sv;-cdc-model;${SRC}/tests/lint/cdc_block.cdc")
  list(GET variant 0 blk)
  list(REMOVE_AT variant 0)
  execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/${blk} ${SRC}/tests/lint/cdc_soc.sv -top cdc_soc -only cdc ${variant}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0
     OR NOT out MATCHES "\\[CDC001\\] 'cdc_soc.f_d' \\(clocked by cdc_soc.fclk\\) is read by logic clocked by cdc_soc.sclk"
     OR NOT out MATCHES "\\[CDC001\\] 'cdc_soc.u_blk.q' \\(clocked by cdc_soc.sclk\\) is read by logic clocked by cdc_soc.fclk"
     OR NOT out MATCHES "2 findings \\(CDC001 2\\)")
    message(FATAL_ERROR "cdc_soc.sv with ${blk} ${variant}:\n${out}${err}")
  endif()
endforeach()

# -cdc-html: one self-contained page with every crossing and reset synchronizer.
execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/cdc_clean.sv -top cdc_clean -only cdc,rdc -cdc-html cdc_clean.html
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ cdc_clean.html page)
foreach(want
    "<title>CDC: cdc_clean</title>"
    "{\"src\":\"cdc_clean.gray\",\"sd\":1,\"dst\":\"cdc_clean.gsync\",\"dd\":0,\"how\":\"2-flop synchronizer, Gray coded\""
    "\"how\":\"one flop only (CDC001)\",\"rule\":\"CDC001\""
    "{\"out\":\"cdc_clean.a_rst_n\",\"d\":0,\"stages\":2,\"flops\":4}")
  string(FIND "${page}" "${want}" pos)
  if(NOT rc EQUAL 0 OR pos EQUAL -1)
    message(FATAL_ERROR "-cdc-html page missing: ${want}\n${err}")
  endif()
endforeach()
if(NOT page MATCHES "\"qual\":\"cdc_clean.req_b_q\"")
  message(FATAL_ERROR "-cdc-html page: no qualifier on the enable-qualified crossing")
endif()
# The schematic shows the logic between a source and its reader.
execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/cdc_bad.sv -top cdc_bad -only cdc -cdc-html cdc_bad.html
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(READ cdc_bad.html bad)
if(NOT bad MATCHES "\"via\":\\[{\"n\":\"cdc_bad.glitchy\",\"g\":1}\\]")
  message(FATAL_ERROR "-cdc-html page: no logic on the path of the CDC003 crossing\n${err}")
endif()
if(page MATCHES "(src|href)=\"?https?:|url\\(https?:|@import")
  message(FATAL_ERROR "-cdc-html page loads something from the network")
endif()

# Port domains from set_input_delay / set_output_delay -clock, with a
# virtual clock: an input read by one flop and an output launched on the
# wrong clock are crossings; the same clock and a synchronized input are not.
run(cdc_io.sv "-top;cdc_io;-only;cdc,rdc;-sdc;${SRC}/tests/lint/cdc_io.sdc;-cdc-report")
foreach(want
    "virtual clock VIO  3 flops  SDC clock VIO"
    "cdc_io.din_s (launched by VIO) (virtual clock VIO) -> cdc_io.s1 (cdc_io.clk)  2-flop synchronizer"
    "[CDC001] 'cdc_io.din_v (launched by VIO)' (clocked by virtual clock VIO) is read by logic clocked by cdc_io.clk through one flop only"
    "[CDC001] 'cdc_io.dout_v' (clocked by cdc_io.clk) is read by logic clocked by virtual clock VIO with no synchronizer"
    "2 findings (CDC001 2)")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_io.sv missing: ${want}\n${out}")
  endif()
endforeach()

# Clock checks: a clock sampled as data (CDC007) and a clock mux made of
# gates (CDC008); a clock gate is not reported, nor a -static select.
run(cdc_clk.sv "-top;cdc_clk;-only;cdc")
foreach(want
    "[CDC007] clock 'cdc_clk.bclk' is read as data by 1 flop clocked by cdc_clk.aclk"
    "[CDC008] clock 'cdc_clk.mclk' of 1 flop is combinational logic of 'cdc_clk.aclk', 'cdc_clk.bclk', 'cdc_clk.sel'"
    "2 findings (CDC007 1, CDC008 1)")
  string(FIND "${out}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "cdc_clk.sv missing: ${want}\n${out}")
  endif()
endforeach()
run(cdc_clk.sv "-top;cdc_clk;-only;cdc;-static;sel")
if(NOT out MATCHES "1 finding \\(CDC007 1\\)")
  message(FATAL_ERROR "cdc_clk.sv -static sel:\n${out}")
endif()

# The findings do not depend on the thread count.
run(cdc_bad.sv "-top;cdc_bad;-only;cdc,RDC001,RDC002,RDC003;-cdc-report;-threads;1")
set(one "${out}")
run(cdc_bad.sv "-top;cdc_bad;-only;cdc,RDC001,RDC002,RDC003;-cdc-report;-threads;8")
if(NOT one STREQUAL out)
  message(FATAL_ERROR "-threads 1 and -threads 8 differ:\n${one}\n---\n${out}")
endif()

# Nothing to report with every default rule on, and -Werror still exits 0.
# (STY001: the file holds its submodule too.)
run(clean.sv "-top;clean;-Werror;-disable;STY001")
if(NOT out MATCHES "0 findings")
  message(FATAL_ERROR "clean.sv is not clean:\n${out}")
endif()

# -Werror exits 1 when there is a finding.
execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/lint.sv -top t -Werror -quiet
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 1 OR out MATCHES "findings")
  message(FATAL_ERROR "-Werror -quiet: rc=${rc}\n${out}")
endif()
# CDC009: a pulse from a fast clock into a slow one is reported; a toggle
# (pulse synchronizer) and a slow-to-fast pulse are not. Periods come from
# create_clock, and from the master for a generated clock.
foreach(sdc cdc_pulse cdc_pulse_div)
  run(cdc_pulse.sv "-top;cdc_pulse;-only;cdc;-sdc;${SRC}/tests/lint/${sdc}.sdc")
  if(NOT out MATCHES "\\[CDC009\\] 'cdc_pulse.pulse_f' \\(clocked by cdc_pulse.fclk\\) is read by logic clocked by cdc_pulse.sclk"
     OR NOT out MATCHES "1 finding \\(CDC009 1\\)")
    message(FATAL_ERROR "cdc_pulse.sv -sdc ${sdc}.sdc:\n${out}")
  endif()
endforeach()
if(NOT out MATCHES "period 2 into one of period 12")
  message(FATAL_ERROR "cdc_pulse.sv: generated clock period:\n${out}")
endif()
# Without periods there is nothing to compare.
run(cdc_pulse.sv "-top;cdc_pulse;-only;cdc")
if(NOT out MATCHES "0 findings")
  message(FATAL_ERROR "cdc_pulse.sv without -sdc:\n${out}")
endif()
# A register struct filled field by field (ports on fields and on a
# concatenation of fields), copied whole and passed whole down: only the
# field that crosses is reported, and the scan-muxed clock is clk_b_func.
run(cdc_regs.sv "-top;cdc_regs;-only;cdc")
if(NOT out MATCHES "\\[CDC001\\] 'cdc_regs.u_c.q' \\(clocked by cdc_regs.clk_a\\) is read by logic clocked by cdc_regs.clk_b_func"
   OR NOT out MATCHES "1 finding \\(CDC001 1\\)")
  message(FATAL_ERROR "cdc_regs.sv:\n${out}")
endif()
# -static takes a pattern.
run(cdc_regs.sv "-top;cdc_regs;-only;cdc;-static;u_c.*")
if(NOT out MATCHES "0 findings")
  message(FATAL_ERROR "cdc_regs.sv -static u_c.*:\n${out}")
endif()
# -cdc-formal: a Gray pointer and a Johnson counter are proved to change one
# bit per clock (no CDC002); a Gray code of a pointer that steps by 2 is
# CDC010, with the step. Without it the two are CDC002.
run(cdc_formal.sv "-top;cdc_formal;-only;cdc")
if(NOT out MATCHES "2 findings \\(CDC002 2\\)")
  message(FATAL_ERROR "cdc_formal.sv without -cdc-formal:\n${out}")
endif()
run(cdc_formal.sv "-top;cdc_formal;-only;cdc;-cdc-formal;-cdc-report;-threads;4")
if(NOT out MATCHES "\\[CDC010\\] 'cdc_formal.skip_q' \\(clocked by cdc_formal.clk_a\\) is synchronized bit by bit as a Gray code, but 2 bits change in one clock, from 0 to 3"
   OR NOT out MATCHES "1 finding \\(CDC010 1\\)"
   OR NOT out MATCHES "gray_q[^\n]*one bit per clock \\(proved, 32 states\\)"
   OR NOT out MATCHES "john_q[^\n]*one bit per clock \\(proved, 8 states\\)")
  message(FATAL_ERROR "cdc_formal.sv -cdc-formal:\n${out}")
endif()
message(STATUS "mlint: ok")

# SoC flow: a block signed off on its own writes the abstract the top-level
# rule test (rules/soc.sv) reads; the checked-in copy must match.
set(abs ${CMAKE_CURRENT_BINARY_DIR}/mlint_blk.abs.json)
run(soc/blk.sv "-top;blk;-q;-abstract-out;${abs}")
file(READ ${abs} got)
file(READ ${SRC}/tests/lint/soc/blk.abs.json want)
if(NOT got STREQUAL want)
  message(FATAL_ERROR "the block abstract changed; regenerate tests/lint/soc/blk.abs.json:\n${got}")
endif()
# -bbox: nothing inside the black box is reported.
execute_process(COMMAND ${MLINT} ${SRC}/tests/lint/rules/soc.sv ${SRC}/tests/lint/soc/blk.sv -top soc -bbox "bl?"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(out MATCHES "blk.sv")
  message(FATAL_ERROR "-bbox: findings inside the black box:\n${out}")
endif()

# -html: one page with the findings and their source.
set(page ${CMAKE_CURRENT_BINARY_DIR}/mlint_report.html)
run(rules/formal.sv "-top;fml;-only;formal;-q;-html;${page}")
file(READ ${page} html)
if(NOT html MATCHES "mlint report" OR NOT html MATCHES "FML002" OR NOT html MATCHES "mem\\[wild\\] <= d")
  message(FATAL_ERROR "-html: the page lacks the findings or their source")
endif()

# -html schematic: the signal of a finding with its drivers and readers. In
# starc.sv the divided clock div2 is driven by a flop clocked by clk and read
# (as a clock) by the flop of q2.
set(page2 ${CMAKE_CURRENT_BINARY_DIR}/mlint_schematic.html)
run(rules/starc.sv "-top;starc;-only;CLK004;-q;-html;${page2}")
file(READ ${page2} html2)
if(NOT html2 MATCHES "\"starc.div2\":\\{\"port\":\"\",\"d\":\\[\\{\"k\":\"flop\",\"at\":\"[^\"]*starc.sv:52\",\"clk\":\"clk\""
   OR NOT html2 MATCHES "\"r\":\\[\\{\"k\":\"flop\",\"at\":\"[^\"]*starc.sv:56\",\"clk\":\"div2\"")
  message(FATAL_ERROR "-html: no schematic data for div2")
endif()
if(NOT html2 MATCHES "function sch\\(")
  message(FATAL_ERROR "-html: no schematic view")
endif()
