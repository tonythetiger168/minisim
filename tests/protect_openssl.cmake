# Interop check without msim's own decryptor: an envelope written by
# msimprotect is opened step by step with the openssl command line tool
# (RSA PKCS#1 v1.5 key_block, then AES-256-CBC data_block with the IV in its
# first 16 bytes, IEEE 1800-2023 34.5.15), and must give back the source text
# between `pragma protect begin and end.
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
function(run)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY ${OUT} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}\n${out}${err}")
  endif()
endfunction()

run(${OPENSSL} genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out priv.pem)
run(${OPENSSL} pkey -in priv.pem -pubout -out pub.pem)
run(${PROTECT} -v1 -key pub.pem -keyowner Acme -keyname acme-1 -digest none
    ${SRC}/examples/protect/secret_alu.sv -o alu.svp)

file(READ ${OUT}/alu.svp env)
string(FIND "${env}" "secret_acc" leak)
if(NOT leak EQUAL -1)
  message(FATAL_ERROR "alu.svp holds cleartext")
endif()
# The base64 text after a block keyword, up to the next `pragma line.
function(get_block keyword var)
  string(REGEX MATCH "`pragma protect ${keyword}\n([A-Za-z0-9+/=\n]+)" m "${env}")
  if(NOT m)
    message(FATAL_ERROR "no ${keyword} in alu.svp")
  endif()
  set(${var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
get_block(key_block kb)
get_block(data_block db)
file(WRITE ${OUT}/kb.b64 "${kb}")
file(WRITE ${OUT}/db.b64 "${db}")
run(${OPENSSL} base64 -d -in kb.b64 -out kb.bin)
run(${OPENSSL} base64 -d -in db.b64 -out db.bin)
run(${OPENSSL} pkeyutl -decrypt -inkey priv.pem -pkeyopt rsa_padding_mode:pkcs1 -in kb.bin -out sk.bin)
file(READ ${OUT}/sk.bin skhex HEX)
file(READ ${OUT}/db.bin dbhex HEX)
string(LENGTH "${skhex}" sklen)
if(NOT sklen EQUAL 64)
  message(FATAL_ERROR "session key is not 32 bytes")
endif()
string(SUBSTRING "${dbhex}" 0 32 ivhex)
# Ciphertext without its IV block: decrypting IV||C under the real IV makes
# the first block garbage, so cut it with openssl itself: decrypt all with
# -nopad, then the cleartext is everything after the first 16 bytes.
run(${OPENSSL} enc -d -aes-256-cbc -nopad -K ${skhex} -iv ${ivhex} -in db.bin -out all.bin)
file(READ ${OUT}/all.bin allhex HEX)
string(SUBSTRING "${allhex}" 32 -1 bodyhex)
# Strip PKCS#7 padding: the last byte says how many bytes to drop.
string(LENGTH "${bodyhex}" n)
math(EXPR last "${n} - 2")
string(SUBSTRING "${bodyhex}" ${last} 2 padhex)
math(EXPR pad "0x${padhex}")
math(EXPR keep "${n} - 2 * ${pad}")
string(SUBSTRING "${bodyhex}" 0 ${keep} bodyhex)

file(READ ${SRC}/examples/protect/secret_alu.sv src)
string(REGEX MATCH "`pragma protect begin\n(.*)`pragma protect end\n" m "${src}")
file(WRITE ${OUT}/want.txt "${CMAKE_MATCH_1}")
file(READ ${OUT}/want.txt wanthex HEX)
if(NOT bodyhex STREQUAL wanthex)
  message(FATAL_ERROR "openssl-decrypted data_block differs from the protected source text")
endif()
