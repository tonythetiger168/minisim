# Protected envelopes end to end (IEEE 1800-2023 clause 34):
#  - msimprotect output has no cleartext, and msim -keyfile runs it with the
#    same output as the plain source;
#  - no key or the wrong key is an error that names the key, not the text;
#  - an error inside protected code is reported without its text;
#  - the VCD has the clear ports but not the protected internals;
#  - -compile falls back to the interpreter;
#  - a protected `include file is found through +incdir+;
#  - an envelope built with the openssl command line tool alone runs too.
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT}/inc)
set(EX ${SRC}/examples/protect)
function(run)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}\n${out}${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
endfunction()
function(run_fail)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(rc EQUAL 0)
    message(FATAL_ERROR "expected a failure: ${ARGN}\n${out}${err}")
  endif()
  set(out "${out}" PARENT_SCOPE)
  set(err "${err}" PARENT_SCOPE)
endfunction()
function(expect text want)
  string(FIND "${text}" "${want}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "expected '${want}' in:\n${text}")
  endif()
endfunction()
function(reject text bad)
  string(FIND "${text}" "${bad}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "'${bad}' leaked into:\n${text}")
  endif()
endfunction()

run(${OPENSSL} genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out acme.pem)
run(${OPENSSL} pkey -in acme.pem -pubout -out acme_pub.pem)
run(${OPENSSL} genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out other.pem)

# Reference: the plain source.
run(${MSIM} ${EX}/secret_alu.sv ${EX}/alu_tb.sv)
set(plain "${out}")
expect("${plain}" "y=")

run(${PROTECT} -key acme_pub.pem -keyowner Acme -keyname acme-1 ${EX}/secret_alu.sv -o alu.svp)
file(READ ${OUT}/alu.svp env)
expect("${env}" "`pragma protect begin_protected")
expect("${env}" "key_keyowner=\"Acme\", key_keyname=\"acme-1\"")
reject("${env}" "secret_acc")
reject("${env}" "always")

run(${MSIM} -keyfile acme.pem alu.svp ${EX}/alu_tb.sv -vcd w.vcd)
if(NOT out STREQUAL plain)
  message(FATAL_ERROR "protected run printed '${out}', plain run '${plain}'")
endif()
file(READ ${OUT}/w.vcd vcd)
expect("${vcd}" " u.y $end")
reject("${vcd}" "secret_acc")

# A named key only opens its own key_keyname (or its own key_public_key).
run(${MSIM} -keyfile acme-1=acme.pem alu.svp ${EX}/alu_tb.sv)

run_fail(${MSIM} alu.svp ${EX}/alu_tb.sv)
expect("${err}" "alu.svp:")
expect("${err}" "no key for this protected envelope (key_keyowner=\"Acme\" key_keyname=\"acme-1\")")
run_fail(${MSIM} -keyfile other.pem alu.svp ${EX}/alu_tb.sv)
expect("${err}" "no key for this protected envelope")

run(${MSIM} -keyfile acme.pem alu.svp ${EX}/alu_tb.sv -compile -o cc)
expect("${err}" "the design has protected code; using the interpreter")
if(EXISTS ${OUT}/cc/design.cpp)
  message(FATAL_ERROR "-compile wrote the protected design as C++")
endif()

# An error in protected code: reported, but with no name or source line.
run(${PROTECT} -key acme_pub.pem ${EX}/bad_alu.sv -o bad.svp)
run_fail(${MSIM} -keyfile acme.pem bad.svp ${EX}/alu_tb.sv)
expect("${err}" "bad.svp: error: in protected code (details hidden)")
reject("${err}${out}" "secret_typo")
reject("${err}${out}" "always")

# A protected `include file, found through +incdir+.
run(${PROTECT} -key acme_pub.pem ${EX}/secret_params.svh -o inc/secret_params.svh)
run(${MSIM} -keyfile acme.pem +incdir+inc ${EX}/inc_tb.sv)
expect("${out}" "k=43")

# An envelope made with the openssl tool only: random session key and IV,
# AES-256-CBC with the IV prepended, RSA PKCS#1 v1.5 key_block, base64.
file(WRITE ${OUT}/body.txt "  initial $display(\"from openssl, %0d\", 6 * 7);\n")
run(${OPENSSL} rand -out sk.bin 32)
run(${OPENSSL} rand -out iv.bin 16)
file(READ ${OUT}/sk.bin skhex HEX)
file(READ ${OUT}/iv.bin ivhex HEX)
run(${OPENSSL} enc -aes-256-cbc -K ${skhex} -iv ${ivhex} -in body.txt -out ct.bin)
run(${OPENSSL} pkeyutl -encrypt -pubin -inkey acme_pub.pem -pkeyopt rsa_padding_mode:pkcs1 -in sk.bin -out kb.bin)
execute_process(COMMAND sh -c "cat iv.bin ct.bin > db.bin" WORKING_DIRECTORY ${OUT})
run(${OPENSSL} base64 -in kb.bin -out kb.b64)
run(${OPENSSL} base64 -in db.bin -out db.b64)
file(SIZE ${OUT}/kb.bin kbn)
file(SIZE ${OUT}/db.bin dbn)
file(READ ${OUT}/kb.b64 kb64)
file(READ ${OUT}/db.b64 db64)
file(WRITE ${OUT}/ossl.sv
  "module ossl;\n"
  "`pragma protect key_keyowner=\"Acme\", key_keyname=\"acme-1\", key_method=\"rsa\"\n"
  "`pragma protect data_method=\"aes256-cbc\"\n"
  "`pragma protect begin_protected\n"
  "`pragma protect encoding=(enctype=\"base64\", line_length=64, bytes=${kbn})\n"
  "`pragma protect key_block\n${kb64}"
  "`pragma protect encoding=(enctype=\"base64\", line_length=64, bytes=${dbn})\n"
  "`pragma protect data_block\n${db64}"
  "`pragma protect end_protected\n"
  "endmodule\n")
run(${MSIM} -keyfile acme-1=acme.pem ossl.sv)
expect("${out}" "from openssl, 42")
