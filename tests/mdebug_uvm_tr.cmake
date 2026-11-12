# End to end with UVM: msim runs tests/debug/uvm_record.sv with recording on,
# uvm-core writes tr_db.log, and mdebug reads it back.
#   cmake -DMSIM=<msim> -DMDEBUG=<mdebug> -DUVM_HOME=<uvm-core> -DSRC=<source dir> -DOUT=<dir> -P mdebug_uvm_tr.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
execute_process(COMMAND ${MSIM} -uvmhome ${UVM_HOME} ${SRC}/tests/debug/uvm_record.sv -top top -vcd rec.vcd
                WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 600)
if(NOT rc EQUAL 0 OR NOT out MATCHES "pass=5 fail=0" OR NOT EXISTS ${OUT}/tr_db.log)
  message(FATAL_ERROR "msim on uvm_record.sv: ${rc}\n${out}${err}")
endif()
execute_process(COMMAND ${MDEBUG} ${SRC}/tests/debug/alu.sv -tr ${OUT}/tr_db.log -trans -at 35000
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "uvm_test_top\\.e\\.ag\\.d\\.drive at 35000:\n  #[0-9]+ t  @30000 to 40000  a='h[0-9a-f]+ b='h[0-9a-f]+ sum='h0  \\(in #[0-9]+ s on uvm_test_top\\.e\\.ag\\.sqr\\.s\\)\n"
   OR NOT out MATCHES "observed at 35000:\n  #[0-9]+ t  @35000 to 40000")
  message(FATAL_ERROR "mdebug -trans on msim's tr_db.log: ${rc}\n${out}${err}")
endif()
