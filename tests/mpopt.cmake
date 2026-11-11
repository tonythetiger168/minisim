# mpopt self-test: the four transformations are found on tests/popt/*.sv, the
# rewritten RTL simulates like the original on random stimulus, a broken
# rewrite is caught, and the result does not depend on the thread count.
#   cmake -DMPOPT=<mpopt> -DSRC=<source dir> -DOUT=<scratch dir> -P mpopt.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})

function(run name)
  execute_process(COMMAND ${MPOPT} ${ARGN}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
  set(rc "${rc}" PARENT_SCOPE)
endfunction()

function(expect name)
  foreach(want ${ARGN})
    string(FIND "${out}" "${want}" pos)
    if(pos EQUAL -1)
      message(FATAL_ERROR "${name}: missing: ${want}\n${out}${err}")
    endif()
  endforeach()
endfunction()

function(check name top)
  run(${name} ${SRC}/tests/popt/${name}.sv -top ${top} -o ${OUT}/${name} -v -verify -cycles 3000 ${ARGN})
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mpopt ${name} exited ${rc}\n${out}${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
  expect(${name} "-verify passed: 3000 cycles")
endfunction()

# Enables, a pipeline (sequential gating), a split block and a memory.
check(pipe pipe)
expect(pipe
  "[CG] pipe: a_q, b_q (32 bits) gated by in_valid"
  "[SCG] pipe: prod_d (32 bits) gated by mpopt_st_L36; stable unless in_valid held in the cycle before"
  "[SCG] pipe: prod (32 bits) gated by mpopt_st_L40; stable unless in_valid || mpopt_st_L36"
  "[CG] pipe: acc (16 bits) gated by acc_en"
  "[CG] pipe: sum_q (16 bits) gated by in_valid && !acc_en"
  "[CG] pipe: flag (1 bit) not gated"
  "[MG] pipe: mem (256 bits) gated by we"
  "[SCG] pipe: rdata (16 bits) gated by mpopt_st_L67; stable unless we || raddr_load"
  "405 register bits, 404 gated (99%)"
  "sequential clock gates:      3 (80 bits)")
file(READ ${OUT}/pipe/pipe.sv text)
foreach(want
    "always_ff @(posedge mpopt_gclk_L25 or negedge rst_n)"
    "mpopt_icg mpopt_icg_L25 (.clk(clk), .en(in_valid), .te(1'b0), .gclk(mpopt_gclk_L25));"
    "if (!rst_n) mpopt_st_L36 <= 1'b1;"
    "// Stage 1: operands load on in_valid (an enable written in the RTL).")
  string(FIND "${text}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "pipe.sv rewrite: missing: ${want}\n${text}")
  endif()
endforeach()
foreach(f files.f mpopt.json mpopt_cells.sv mpopt_tb.sv)
  if(NOT EXISTS ${OUT}/pipe/${f})
    message(FATAL_ERROR "pipe: ${f} was not written")
  endif()
endforeach()

# Operand isolation, signed and unsigned, in always_ff, assign and always_comb.
check(iso iso)
expect(iso
  "[OI] iso: operator * (24 bits) isolated by !rst && use_mul"
  "[OI] iso: operator * (24 bits) isolated by use_mul"
  "[OI] iso: operator / (12 bits) isolated by mode == 2'd3"
  "operators isolated:          4")
file(READ ${OUT}/iso/iso.sv text)
# Named isolation signals of the operand's own width in always_ff and assign;
# the expression rewritten in place in always_comb.
foreach(want
    "wire signed [11:0] mpopt_iso_L22_a = (!rst && use_mul) !== 1'b0 ? sa : $signed(1'b0);"
    "if (use_mul) sp <= mpopt_iso_L22_a * mpopt_iso_L22_b;"
    "wire [23:0] mpopt_iso_L30_b = use_mul !== 1'b0 ? (ub + 12'd3) : 1'b0;"
    "assign y = use_mul ? mpopt_iso_L30_a * mpopt_iso_L30_b : {12'd0, ua};"
    "q = ((mode == 2'd3) !== 1'b0 ? ua : 1'b0) / ((mode == 2'd3) !== 1'b0 ? (ub | 12'd1) : 1'b0);")
  string(FIND "${text}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "iso.sv: missing: ${want}\n${text}")
  endif()
endforeach()
# -iso_inline: no new signals; a plain signal keeps its width inside a concatenation.
run(iso ${SRC}/tests/popt/iso.sv -top iso -o ${OUT}/iso_inline -iso_inline -verify)
file(READ ${OUT}/iso_inline/iso.sv text)
string(FIND "${text}" "$signed({((!rst && use_mul) !== 1'b0 ? sa : $signed(1'b0))}) * $signed({((!rst && use_mul) !== 1'b0 ? sb : $signed(1'b0))})" pos)
string(FIND "${text}" "mpopt_iso_" named)
if(pos EQUAL -1 OR NOT named EQUAL -1 OR NOT rc EQUAL 0)
  message(FATAL_ERROR "-iso_inline: unexpected rewrite or -verify failed\n${out}${err}${text}")
endif()
# -no_iso and -iso_add.
run(iso ${SRC}/tests/popt/iso.sv -top iso -no_iso)
expect(iso "operators isolated:          0")
run(iso ${SRC}/tests/popt/iso.sv -top iso -o ${OUT}/iso_plain -iso_plain -verify)
file(READ ${OUT}/iso_plain/iso.sv text)
string(FIND "${text}" "wire [11:0] mpopt_iso_L30_a = use_mul ? ua : 1'b0;" pos)
if(pos EQUAL -1 OR NOT rc EQUAL 0)
  message(FATAL_ERROR "-iso_plain: unexpected rewrite or -verify failed\n${out}${err}${text}")
endif()
run(iso ${SRC}/tests/popt/iso.sv -top iso -iso_add 16)
expect(iso "operators isolated:          6" "operator + (24 bits)")

# Two parameterizations, a generate loop, a synchronous reset, Verilog-2001.
check(top top)
expect(top
  "[CG] stage: q (16 bits) gated by rst || en"
  "[SCG] stage: q2 (16 bits) gated by mpopt_st_L15 || rst"
  "[CG] top: t (8 bits x 2) gated by en[2 + i]"
  "2 modules, 3 instances, 72 register bits, 72 gated (100%)")

# What must be left alone, and the corner cases.
check(edge edge_cases -test_enable scan_en)
expect(edge
  "[CG] edge_cases: mac_q (8 bits) not gated; the block has text that comes from a macro"
  "[CG] edge_cases: cnt (8 bits) gated by en"
  "[SCG] edge_cases: follow (8 bits) gated by mpopt_st_L41; stable unless en held"
  "neg_q (8 bits) not gated; the clock is a falling edge"
  "blk_q, tmp (16 bits) not gated; the block has a blocking assignment"
  "[CG] edge_cases: lab_a, lab_b (16 bits) gated by en || en2"
  "[CG] edge_cases: hi, lo (8 bits) gated by rst || (en & en2)"
  "casez_q (8 bits) gated by (op ==? (2'b1?)) || (op ==? 2'b01)"
  "same_q (8 bits) not gated; it loads every cycle"
  "fn_q (8 bits) not gated"
  "[MG] edge_cases: arr (32 bits) gated by en2"
  "s5 (8 bits) gated by mpopt_st_L105 || rst"
  "s6 (8 bits) not gated; it loads every cycle and the enable chain is too deep"
  "[CG] edge_cases: nb_a (8 bits) gated by en"
  "[CG] edge_cases: nb_b (8 bits) gated by en2"
  "wide (16 bits) gated by (rst || take) && (mpopt_st_L120 || rst)")
file(READ ${OUT}/edge/edge.sv text)
string(FIND "${text}" ".te(scan_en)" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "edge.sv: -test_enable was not connected\n${text}")
endif()

# A broken rewrite must be caught: start a stability flop at 0 instead of 1.
string(REPLACE "mpopt_st_L41 <= 1'b1" "mpopt_st_L41 <= 1'b0" bad "${text}")
file(WRITE ${OUT}/edge/edge.sv "${bad}")
run(edge ${SRC}/tests/popt/edge.sv -top edge_cases -o ${OUT}/edge -verify_only -cycles 3000 -test_enable scan_en)
if(rc EQUAL 0 OR NOT err MATCHES "-verify FAILED")
  message(FATAL_ERROR "a broken stability flop was not caught (exit ${rc})\n${out}${err}")
endif()
# And an enable that is too narrow.
string(REPLACE ".en(en || en2)" ".en(en)" bad "${text}")
file(WRITE ${OUT}/edge/edge.sv "${bad}")
run(edge ${SRC}/tests/popt/edge.sv -top edge_cases -o ${OUT}/edge -verify_only -cycles 3000 -test_enable scan_en)
if(rc EQUAL 0 OR NOT err MATCHES "-verify FAILED")
  message(FATAL_ERROR "a wrong enable was not caught (exit ${rc})\n${out}${err}")
endif()

# The same files and the same report on one thread and on many.
foreach(n 1 2 7)
  run(threads ${SRC}/tests/popt/pipe.sv ${SRC}/tests/popt/iso.sv ${SRC}/tests/popt/top.sv
      ${SRC}/tests/popt/edge.sv -top pipe -top iso -top top -top edge_cases
      -o ${OUT}/t${n} -json ${OUT}/t${n}.json -threads ${n} -v)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "mpopt -threads ${n} exited ${rc}\n${out}${err}")
  endif()
  string(REPLACE "${OUT}/t${n}" "OUT" report "${out}")
  file(WRITE ${OUT}/t${n}.txt "${report}")
endforeach()
foreach(n 2 7)
  foreach(f pipe.sv iso.sv top.sv edge.sv mpopt.json)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/t1/${f} ${OUT}/t${n}/${f} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "-threads ${n}: ${f} differs from -threads 1")
    endif()
  endforeach()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/t1.txt ${OUT}/t${n}.txt RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "-threads ${n}: the report differs from -threads 1")
  endif()
endforeach()

# A library clock gate instead of the model, and the switches.
run(icg ${SRC}/tests/popt/pipe.sv -top pipe -o ${OUT}/icg -icg_cell CKLNQD1:CP,E,TE,Q -no_seq -no_split -min_bits 8)
expect(icg "sequential clock gates:      0 (0 bits)")
file(READ ${OUT}/icg/pipe.sv text)
string(FIND "${text}" "CKLNQD1 mpopt_icg_L25 (.CP(clk), .E(in_valid), .TE(1'b0), .Q(mpopt_gclk_L25));" pos)
if(pos EQUAL -1 OR EXISTS ${OUT}/icg/mpopt_cells.sv)
  message(FATAL_ERROR "-icg_cell: the library cell was not used\n${text}")
endif()
string(FIND "${text}" "mpopt_gclk_L63" pos)   # raddr is 4 bits: under -min_bits 8
if(NOT pos EQUAL -1)
  message(FATAL_ERROR "-min_bits 8 gated a 4-bit register\n${text}")
endif()

# -prove: mequiv proves the rewrite; with sequential gates, the rewrite without them.
if(EXISTS ${MEQUIV})
  run(prove ${SRC}/tests/popt/top.sv -top top -o ${OUT}/prove_top -no_seq -prove)
  expect(prove "-prove passed: mequiv proved the rewritten RTL equivalent to the original")
  run(prove ${SRC}/tests/popt/pipe.sv -top pipe -o ${OUT}/prove_pipe -prove -threads 2)
  expect(prove "-prove passed: mequiv proved the rewrite without sequential gates (in prove/)"
               "sequential gates are outside the proof")
  if(NOT rc EQUAL 0 OR NOT EXISTS ${OUT}/prove_pipe/prove/files.f OR NOT EXISTS ${OUT}/prove_pipe/prove.json)
    message(FATAL_ERROR "-prove: exit ${rc} or files missing\n${out}${err}")
  endif()
  # A wrong enable in the rewritten RTL must fail the proof.
  file(READ ${OUT}/prove_top/top.sv text)
  string(REPLACE ".en(rst || en)" ".en(rst)" bad "${text}")
  if(bad STREQUAL text)
    message(FATAL_ERROR "-prove: the enable to break was not found\n${text}")
  endif()
  file(WRITE ${OUT}/prove_top/top.sv "${bad}")
  execute_process(COMMAND ${MEQUIV} -q -top top -ref ${SRC}/tests/popt/top.sv -impl -f ${OUT}/prove_top/files.f
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL 1)
    message(FATAL_ERROR "-prove: mequiv did not catch a wrong enable (exit ${rc})")
  endif()
endif()

# -power: mpower before and after, on the activity of the -verify run.
if(EXISTS ${MPOWER})
  run(power ${SRC}/tests/popt/pipe.sv -top pipe -o ${OUT}/power_pipe -power)
  expect(power "-verify passed" "-power: before and after" "clock network")
  string(REGEX MATCH "total +([0-9.]+) uW  ->  +([0-9.]+) uW" m "${out}")
  if(NOT rc EQUAL 0 OR NOT m OR NOT CMAKE_MATCH_2 LESS CMAKE_MATCH_1)
    message(FATAL_ERROR "-power: pipe did not get cheaper\n${out}${err}")
  endif()
  # Operand isolation alone must show a saving too.
  run(power ${SRC}/tests/popt/iso.sv -top iso -o ${OUT}/power_iso -no_cg -power)
  string(REGEX MATCH "combinational +([0-9.]+) uW  ->  +([0-9.]+) uW" m "${out}")
  if(NOT rc EQUAL 0 OR NOT m OR NOT CMAKE_MATCH_2 LESS CMAKE_MATCH_1)
    message(FATAL_ERROR "-power: isolation did not lower the combinational power\n${out}${err}")
  endif()
endif()
