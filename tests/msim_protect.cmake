# Protected envelopes end to end (IEEE 1735-2023 version 3 by default, and
# IEEE 1800-2023 clause 34 with msimprotect -v1):
#  - msimprotect output has no cleartext, and msim -keyfile runs it with the
#    same output as the plain source;
#  - no key or the wrong key is an error that names the key, not the text;
#  - an error inside protected code is reported without its text;
#  - the VCD has the clear ports but not the protected internals;
#  - -compile falls back to the interpreter;
#  - a protected `include file is found through +incdir+;
#  - an envelope built with the openssl command line tool alone runs too
#    (one of each kind: an independent check of the cryptography);
#  - version 3: tampering fails the signature, keyrings, rights (decryption,
#    visibility, coverage, min_version), viewports, and mlint / mdft / -cov
#    keeping protected code out of their output.
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
expect("${vcd}" "$scope module u $end")
expect("${vcd}" " y $end")
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

# The same source as a clause 34 envelope (-v1) still runs.
run(${PROTECT} -v1 -key acme_pub.pem -keyowner Acme -keyname acme-1 ${EX}/secret_alu.sv -o alu_v1.svp)
file(READ ${OUT}/alu_v1.svp env1)
expect("${env1}" "digest_block")
reject("${env1}" "version=3")
run(${MSIM} -keyfile acme.pem alu_v1.svp ${EX}/alu_tb.sv)
if(NOT out STREQUAL plain)
  message(FATAL_ERROR "-v1 run printed '${out}', plain run '${plain}'")
endif()

# ------------------------------------------------------------ version 3
expect("${env}" "`pragma protect version=3\n")
expect("${env}" "`pragma protect data_method=\"aes256-cbc-hmac-sha512\"\n")
expect("${env}" "`pragma protect key_method=\"rsa_no_padding\"\n")
expect("${env}" "`pragma protect end_protected=\"")

# A changed character anywhere in the envelope fails the signature check, and
# nothing of the source is shown.
string(REPLACE "key_method=\"rsa_no_padding\"\n`pragma protect key_block\n" "key_method=\"rsa_no_padding\"\n`pragma protect key_block\n" e2 "${env}")
string(REPLACE "encrypt_agent=\"msimprotect\"" "encrypt_agent=\"msimprotecT\"" e2 "${env}")
file(WRITE ${OUT}/tamper.svp "${e2}")
run_fail(${MSIM} -keyfile Acme:acme-1=acme.pem tamper.svp ${EX}/alu_tb.sv)
expect("${err}" "tamper.svp:8: signature verification failed")
reject("${err}${out}" "secret_acc")

# Keyrings: msim reads private keys by key_keyowner / key_keyname; msimprotect
# reads public keys in the IEEE 1735 6.3.1 key exchange format.
file(WRITE ${OUT}/private.keyring "# owner  name  key\n\"Acme\"  acme-1  acme.pem\nOther other-1 other.pem\n")
run(${MSIM} -keyring private.keyring alu.svp ${EX}/alu_tb.sv)
expect("${out}" "y=")
file(WRITE ${OUT}/dup.keyring "Acme acme-1 acme.pem\nAcme acme-1 other.pem\n")
run_fail(${MSIM} -keyring dup.keyring alu.svp ${EX}/alu_tb.sv)
expect("${err}" "two keys named key_keyowner=\"Acme\" key_keyname=\"acme-1\"")
run(${PROTECT} -export-key acme_pub.pem -keyowner "Acme Corp" -keyname core-2026 -o acme.keys)
run(${PROTECT} -export-key other.pem -keyowner Other -keyname other-1)
file(APPEND ${OUT}/acme.keys "${out}")
file(READ ${OUT}/acme.keys keys)
expect("${keys}" "`pragma protect key_keyowner = \"Acme Corp\"\n`pragma protect key_method = \"rsa_no_padding\"\n`pragma protect key_keyname = \"core-2026\"\n`pragma protect key_public_key\n")
# No -key: one tool block for every key in the keyring.
run(${PROTECT} -keyring acme.keys ${EX}/secret_core.sv -o core_all.svp)
file(READ ${OUT}/core_all.svp ca)
expect("${ca}" "key_keyowner=\"Acme Corp\", key_keyname=\"core-2026\"")
expect("${ca}" "key_keyowner=\"Other\", key_keyname=\"other-1\"")
run(${MSIM} -keyfile Other:other-1=other.pem core_all.svp ${EX}/core_tb.sv)
expect("${out}" "q=")
set(coreplain "${out}")

# Rights (control pragmas in the tool block), one envelope per setting.
foreach(vis none interface full)
  run(${PROTECT} -key acme_pub.pem -keyowner Acme -keyname acme-1 -control visibility=${vis} ${EX}/secret_core.sv -o core_${vis}.svp)
  run(${MSIM} -keyfile Acme:acme-1=acme.pem core_${vis}.svp ${EX}/core_tb.sv -vcd core_${vis}.vcd)
  if(NOT out STREQUAL coreplain)
    message(FATAL_ERROR "visibility=${vis} printed '${out}', expected '${coreplain}'")
  endif()
  file(READ ${OUT}/core_${vis}.vcd v_${vis})
endforeach()
# The viewport pragma before `stage` makes it visible at every level.
foreach(vis none interface full)
  expect("${v_${vis}}" " stage $end")
endforeach()
reject("${v_none}" "hidden_sum")
reject("${v_none}" " 8 # d $end")
reject("${v_interface}" "hidden_sum")
expect("${v_interface}" "$scope module u $end")
expect("${v_interface}" " d $end")
expect("${v_interface}" " q $end")
expect("${v_full}" " hidden_sum $end")

# Coverage: no lines or toggles from protected code, unless the rights allow it.
run(${MSIM} -keyfile Acme:acme-1=acme.pem core_none.svp ${EX}/core_tb.sv -cov cov_none.json)
file(READ ${OUT}/cov_none.json cn)
reject("${cn}" "hidden_sum")
string(REGEX MATCH "\"files\": \\[[^]]*\\]" files "${cn}")
string(REGEX REPLACE "^\"files\": \\[|\\]$" "" files "${files}")
string(REPLACE ", " ";" files "${files}")
list(FIND files "\"${OUT}/core_none.svp\"" coreidx)
if(NOT coreidx EQUAL -1)
  string(FIND "${cn}" "[${coreidx}, " hit)
  if(NOT hit EQUAL -1)
    message(FATAL_ERROR "line coverage recorded inside protected code:\n${cn}")
  endif()
endif()
run(${PROTECT} -key acme_pub.pem -keyowner Acme -keyname acme-1 -control coverage=on ${EX}/secret_core.sv -o core_cov.svp)
run(${MSIM} -keyfile Acme:acme-1=acme.pem core_cov.svp ${EX}/core_tb.sv -cov cov_on.json)
file(READ ${OUT}/cov_on.json con)
expect("${con}" "core_cov.svp")
reject("${con}" "hidden_sum")

# decryption = "deny", and a minimum minisim version.
run(${PROTECT} -key acme_pub.pem -keyowner Acme -keyname acme-1 -control decryption=deny ${EX}/secret_core.sv -o core_deny.svp)
run_fail(${MSIM} -keyfile Acme:acme-1=acme.pem core_deny.svp ${EX}/core_tb.sv)
expect("${err}" "do not allow decryption")
run(${PROTECT} -key acme_pub.pem -keyowner Acme -keyname acme-1 -control min_version=99.0 ${EX}/secret_core.sv -o core_new.svp)
run_fail(${MSIM} -keyfile Acme:acme-1=acme.pem core_new.svp ${EX}/core_tb.sv)
expect("${err}" "needs minisim 99.0 or later")

# Licensed rights: visibility = license("vis") ? "full" : "none".
file(WRITE ${OUT}/core_lic.sv
  "`pragma protect version=3\n"
  "`pragma protect begin_toolblock\n"
  "`pragma protect key_keyowner=\"Acme\", key_keyname=\"acme-1\"\n"
  "`pragma protect key_method=\"rsa_no_padding\"\n"
  "`pragma protect control visibility = license(\"acme_debug\") ? \"full\" : \"none\"\n"
  "`pragma protect end_toolblock\n")
file(READ ${EX}/secret_core.sv core_src)
file(APPEND ${OUT}/core_lic.sv "${core_src}")
run(${PROTECT} -keyring acme.keys -key acme_pub.pem -keyowner Acme -keyname acme-1 core_lic.sv -o core_lic.svp)
run(${MSIM} -keyfile Acme:acme-1=acme.pem core_lic.svp ${EX}/core_tb.sv -vcd lic_off.vcd)
file(READ ${OUT}/lic_off.vcd lo)
reject("${lo}" "hidden_sum")
run(${MSIM} -keyfile Acme:acme-1=acme.pem -license acme_debug core_lic.svp ${EX}/core_tb.sv -vcd lic_on.vcd)
file(READ ${OUT}/lic_on.vcd lon)
expect("${lon}" "hidden_sum")

# msim's own messages about protected code (a default assertion failure)
# name neither the label nor the line (IEEE 1735 9.2.1); the IP's own
# $display with %m still prints (9.2.2).
file(WRITE ${OUT}/ap.sv
  "module ap(input logic clk);\n"
  "`pragma protect begin\n"
  "  logic secret_flag = 0;\n"
  "  a_secret: assert property (@(posedge clk) secret_flag);\n"
  "  initial #12 $display(\"%m here\");\n"
  "`pragma protect end\n"
  "endmodule\n"
  "module tb; logic clk = 0; always #5 clk = ~clk; ap u(clk); initial #20 $finish; endmodule\n")
run(${PROTECT} -key acme_pub.pem -keyowner Acme -keyname acme-1 ap.sv -o ap.svp)
run(${MSIM} -keyfile Acme:acme-1=acme.pem ap.svp)
expect("${out}" "Error: \"ap.svp\", protected code: tb.u.<protected assertion>: at time 5")
expect("${out}" "tb.u here")
reject("${out}${err}" "a_secret")

# mlint and mdft open protected IP with the same options and keep its
# findings and names to themselves.
if(MLINT)
  run(${MLINT} ${EX}/secret_core.sv ${EX}/core_tb.sv)
  expect("${out}" "[L002]")
  run(${MLINT} -keyfile Acme:acme-1=acme.pem core_none.svp ${EX}/core_tb.sv)
  reject("${out}${err}" "[L002]")
  reject("${out}${err}" "core_tb.u.q")
  expect("${out}" "mlint: 1 finding in protected code (details hidden)")
  run(${MLINT} -keyfile Acme:acme-1=acme.pem core_full.svp ${EX}/core_tb.sv)
  expect("${out}" "[L002]")
  run_fail(${MLINT} core_none.svp ${EX}/core_tb.sv)
  expect("${err}" "no key for this protected envelope")
endif()
if(MDFT)
  run(${MDFT} -keyfile Acme:acme-1=acme.pem core_none.svp ${EX}/core_tb.sv)
  reject("${out}${err}" "core_tb.u.clk")
  reject("${out}${err}" "secret_core")
  # The finding sits in the plain testbench, so it shows; the clock it shares
  # with the protected core is named <protected>.
  expect("${out}" "'<protected>'")
endif()

# A version 3 envelope made with the openssl tool only (IEEE 1735 5.4): a
# 64-byte root key in the low bytes of an RSA block encrypted with no
# padding, AES-256-CBC with the first 32 bytes, HMAC-SHA512 with the last 32
# over the envelope text up to end_protected.
file(WRITE ${OUT}/body3.txt "  initial $display(\"v3 from openssl, %0d\", 6 * 7);\n")
run(${OPENSSL} rand -out root.bin 64)
run(${OPENSSL} rand -out pad.bin 191)
execute_process(COMMAND sh -c "printf '\\000' > blk.bin && cat pad.bin root.bin >> blk.bin" WORKING_DIRECTORY ${OUT})
file(SIZE ${OUT}/blk.bin blkn)
if(NOT blkn EQUAL 256)
  message(FATAL_ERROR "RSA block is ${blkn} bytes")
endif()
run(${OPENSSL} pkeyutl -encrypt -pubin -inkey acme_pub.pem -pkeyopt rsa_padding_mode:none -in blk.bin -out kb3.bin)
file(READ ${OUT}/root.bin roothex HEX)
string(SUBSTRING "${roothex}" 0 64 aeshex)
string(SUBSTRING "${roothex}" 64 64 machex)
run(${OPENSSL} rand -out iv3.bin 16)
file(READ ${OUT}/iv3.bin iv3hex HEX)
run(${OPENSSL} enc -aes-256-cbc -K ${aeshex} -iv ${iv3hex} -in body3.txt -out ct3.bin)
execute_process(COMMAND sh -c "cat iv3.bin ct3.bin > db3.bin" WORKING_DIRECTORY ${OUT})
run(${OPENSSL} base64 -in kb3.bin -out kb3.b64)
run(${OPENSSL} base64 -in db3.bin -out db3.b64)
file(READ ${OUT}/kb3.b64 kb3)
file(READ ${OUT}/db3.b64 db3)
set(env3
  "`pragma protect begin_protected\n"
  "`pragma protect version=3\n"
  "`pragma protect encrypt_agent=\"openssl\"\n"
  "`pragma protect data_method=\"aes256-cbc-hmac-sha512\"\n"
  "`pragma protect begin_toolblock\n"
  "`pragma protect key_keyowner=\"Acme\", key_keyname=\"acme-1\"\n"
  "`pragma protect key_method=\"rsa_no_padding\"\n"
  "`pragma protect key_block\n${kb3}"
  "`pragma protect control visibility=\"full\"\n"
  "`pragma protect end_toolblock\n"
  "`pragma protect data_block\n${db3}"
  "`pragma protect end_protected")
string(CONCAT env3 ${env3})
file(WRITE ${OUT}/signed3.txt "${env3}")
run(${OPENSSL} dgst -sha512 -mac HMAC -macopt hexkey:${machex} -binary -out sig3.bin signed3.txt)
run(${OPENSSL} base64 -A -in sig3.bin)
string(STRIP "${out}" sig3)
file(WRITE ${OUT}/ossl3.sv "module ossl3;\n${env3}=\"${sig3}\"\nendmodule\n")
run(${MSIM} -keyfile Acme:acme-1=acme.pem ossl3.sv)
expect("${out}" "v3 from openssl, 42")
# And the other way: msimprotect's envelope, checked with the openssl tool.
string(FIND "${env}" "`pragma protect begin_protected" b0)
string(FIND "${env}" "`pragma protect end_protected=\"" e0)
math(EXPR e1 "${e0} + 29")
math(EXPR elen "${e1} - ${b0}")
string(SUBSTRING "${env}" ${b0} ${elen} signedm)
string(SUBSTRING "${env}" ${e1} 200 sigm)
string(REGEX REPLACE "^=\"([^\"]*)\".*" "\\1" sigm "${sigm}")
string(REGEX MATCH "key_block\n([A-Za-z0-9+/=\n]*)`" kbm "${env}")
string(REGEX REPLACE "^key_block\n|`$" "" kbm "${kbm}")
file(WRITE ${OUT}/kbm.b64 "${kbm}")
run(${OPENSSL} base64 -d -in kbm.b64 -out kbm.bin)
run(${OPENSSL} pkeyutl -decrypt -inkey acme.pem -pkeyopt rsa_padding_mode:none -in kbm.bin -out blkm.bin)
file(READ ${OUT}/blkm.bin blkmhex HEX)
string(LENGTH "${blkmhex}" bl)
math(EXPR macoff "${bl} - 64")
string(SUBSTRING "${blkmhex}" ${macoff} 64 macm)
file(WRITE ${OUT}/signedm.txt "${signedm}")
run(${OPENSSL} dgst -sha512 -mac HMAC -macopt hexkey:${macm} -binary -out sigm.bin signedm.txt)
run(${OPENSSL} base64 -A -in sigm.bin)
string(STRIP "${out}" sigcheck)
if(NOT sigcheck STREQUAL sigm)
  message(FATAL_ERROR "openssl computes signature ${sigcheck}, msimprotect wrote ${sigm}")
endif()
