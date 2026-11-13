# Transaction recording without UVM (expr.hpp trrec): Questa's
# $create_transaction_stream / $begin_transaction / $add_attribute /
# $end_transaction and Verdi's $fsdbTrans_* write the text format of UVM's
# uvm_text_tr_database, so mdebug -tr reads them as it reads tr_db.log.
# The interpreter and -compile write the same file.
#   cmake -DMSIM=<msim> -DMDEBUG=<mdebug> -DSRC=<source dir> -DOUT=<dir> -P msim_tr.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
set(SV ${SRC}/tests/debug/tr_record.sv)
execute_process(COMMAND ${MSIM} ${SV} WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS ${OUT}/transactions.log)
  message(FATAL_ERROR "msim exited ${rc} or wrote no transactions.log\n${err}")
endif()
file(READ ${OUT}/transactions.log tr)
foreach(want "CREATE_STREAM @0 {NAME:drive T:TVM SCOPE:tb.bus STREAM:1}"
             "OPEN_RECORDER @25 {TXH:3 STREAM:1 NAME:write TIME:25"
             "SET_ATTR @25 {TXH:3 NAME:addr VALUE:14   RADIX:UVM_HEX BITS=8}"
             "SET_ATTR @25 {TXH:3 NAME:wdata VALUE:eb   RADIX:UVM_HEX BITS=8}"
             "SET_ATTR @25 {TXH:3 NAME:status VALUE:ok   RADIX:UVM_STRING"
             "CLOSE_RECORDER @35 {TXH:3 TIME=35}"
             "NAME:seen T:TVM SCOPE:tb.mon"
             "LINK @55 {TXH1:6 TXH2:7 RELATION=child}"
             "SET_ATTR @55 {TXH:7 NAME:byte VALUE:5a")
  string(FIND "${tr}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "transactions.log lacks '${want}':\n${tr}")
  endif()
endforeach()

execute_process(COMMAND ${MDEBUG} ${SV} -tr ${OUT}/transactions.log -trans -at 60 RESULT_VARIABLE rc OUTPUT_VARIABLE o ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT o MATCHES "tb.mon.seen at 60:\n  #3 burst  @55 to 75\n  #4 beat  @55 to 65  byte='h5a  \\(in #3 burst on tb.mon.seen\\)")
  message(FATAL_ERROR "mdebug -trans -at 60 exited ${rc}:\n${o}${err}")
endif()
execute_process(COMMAND ${MDEBUG} ${SV} -tr ${OUT}/transactions.log -trans RESULT_VARIABLE rc OUTPUT_VARIABLE o)
if(NOT o MATCHES "tb.bus.drive  3 transactions  \\(5 to 55\\)\ntb.mon.seen  2 transactions  \\(55 to 75\\)")
  message(FATAL_ERROR "mdebug -trans:\n${o}")
endif()

# -compile and -tr FILE: the same records.
execute_process(COMMAND ${MSIM} ${SV} -compile -o ${OUT}/simv.d -tr ${OUT}/c.log WORKING_DIRECTORY ${OUT}
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT EXISTS ${OUT}/c.log)
  message(FATAL_ERROR "msim -compile exited ${rc} or wrote no c.log\n${err}")
endif()
file(READ ${OUT}/c.log c)
if(NOT c STREQUAL tr)
  message(FATAL_ERROR "-compile records differ\n--- interpreter\n${tr}--- compiled\n${c}")
endif()
