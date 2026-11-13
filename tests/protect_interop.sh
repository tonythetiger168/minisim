#!/bin/sh
# Interop with other IEEE 1735 tools: envelopes msim did not make.
#
#   protect_interop.sh <msim> <openssl> <source dir> <work dir>
#
# Every envelope here is built with the openssl command line tool alone, in
# the shapes another vendor's encryptor emits, and none of them passes
# through msimprotect. What this can show is what a published format and a
# public key allow: that msim opens a version 3 envelope and an IEEE 1800
# clause 34 envelope it did not make, picks its own tool block out of
# several, honours the rights in it, and reads CR LF files. What it cannot
# show is opening an envelope encrypted only for a commercial tool: that
# needs that vendor's private key, which minisim does not have.
set -eu
MSIM=$1; OPENSSL=$2; SRC=$3; OUT=$4
T=$SRC/tests/protect
rm -rf "$OUT"; mkdir -p "$OUT"; cd "$OUT" || exit 1

fail() { echo "FAIL: $*"; echo "--- output"; cat log 2>/dev/null; exit 1; }
want() { grep -qF -- "$1" log || fail "expected: $1"; }
hex() { od -An -v -tx1 "$1" | tr -d ' \n'; }

"$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out acme.pem 2>/dev/null
"$OPENSSL" pkey -in acme.pem -pubout -out acme_pub.pem

# The cleartext run everything is compared against.
"$MSIM" "$T/interop_ip.sv" "$T/interop_tb.sv" > plain.log 2>&1 || fail "the plain run failed"
grep -qF "tb: y=55" plain.log || fail "the plain run should print tb: y=55"

# ---- a version 3 envelope another eTool could have written ----------------
# The root key goes in the low 64 bytes of a random number below the modulus
# (5.4.3); its first half is the AES-256-CBC key of the data block and its
# second half the HMAC-SHA512 key of the signature (5.4.1).
"$OPENSSL" rand -out root.bin 64
R=$(hex root.bin)
AESK=$(printf %s "$R" | cut -c1-64)
MACK=$(printf %s "$R" | cut -c65-128)
MODB=$(( $("$OPENSSL" rsa -pubin -in acme_pub.pem -noout -modulus | sed 's/Modulus=//' | wc -c) / 2 ))
printf '\000' > pt.bin
"$OPENSSL" rand $(( MODB - 65 )) >> pt.bin
cat root.bin >> pt.bin
"$OPENSSL" pkeyutl -encrypt -pubin -inkey acme_pub.pem -pkeyopt rsa_padding_mode:none -in pt.bin -out kb.bin
"$OPENSSL" base64 -in kb.bin -out kb.b64
"$OPENSSL" rand -out iv.bin 16
"$OPENSSL" enc -aes-256-cbc -K "$AESK" -iv "$(hex iv.bin)" -in "$T/interop_ip.sv" -out ct.bin
cat iv.bin ct.bin > data.bin
"$OPENSSL" base64 -in data.bin -out db.b64

# A tool block for a vendor we have no key for comes first: msim must walk
# past it to its own (5.4.2), and must not be upset by the agent and author
# pragmas or by a right it knows.
{
  echo '`pragma protect begin_protected'
  echo '`pragma protect version=3'
  echo '`pragma protect encrypt_agent="other_vendor_protect", encrypt_agent_info="4.2"'
  echo '`pragma protect author="Third Party IP", author_info="core rev 2"'
  echo '`pragma protect data_method="aes256-cbc-hmac-sha512"'
  echo '`pragma protect begin_toolblock'
  echo '`pragma protect key_keyowner="Other Vendor", key_keyname="ov-2026"'
  echo '`pragma protect key_method="rsa_no_padding"'
  echo '`pragma protect key_block'
  echo 'Zm9yIGEgdG9vbCB3ZSBoYXZlIG5vIGtleSBmb3I='
  echo '`pragma protect end_toolblock'
  echo '`pragma protect begin_toolblock'
  echo '`pragma protect key_keyowner="Acme", key_keyname="acme-1"'
  echo '`pragma protect key_method="rsa_no_padding"'
  echo '`pragma protect key_block'
  cat kb.b64
  echo '`pragma protect control visibility = "interface"'
  echo '`pragma protect end_toolblock'
  echo '`pragma protect data_block'
  cat db.b64
  printf '`pragma protect end_protected'
} > body.txt
"$OPENSSL" dgst -sha512 -mac HMAC -macopt hexkey:"$MACK" -binary -out sig.bin body.txt
{ cat body.txt; printf '="%s"\n' "$("$OPENSSL" base64 -A -in sig.bin)"; } > v3.svp

"$MSIM" -keyfile Acme:acme-1=acme.pem v3.svp "$T/interop_tb.sv" > log 2>&1 || fail "the version 3 envelope did not run"
want "tb: y=55"
# The rights of that tool block hold: inner_state is not a port, so it stays
# out of the waveform even though the envelope is not one of ours.
"$MSIM" -keyfile Acme:acme-1=acme.pem v3.svp "$T/interop_tb.sv" -vcd w.vcd > log 2>&1 || fail "the -vcd run failed"
if grep -qF inner_state w.vcd; then fail "the trace names a signal the rights hide"; fi

# The same envelope with CR LF line ends: the signature is over the text with
# the carriage returns removed (5.8), so it must still verify.
sed 's/$/\r/' v3.svp > v3_crlf.svp
"$MSIM" -keyfile Acme:acme-1=acme.pem v3_crlf.svp "$T/interop_tb.sv" > log 2>&1 || fail "the CR LF envelope did not run"
want "tb: y=55"

# With no key for either tool block, msim says whose key it wants and shows
# nothing of the text.
"$MSIM" v3.svp "$T/interop_tb.sv" > log 2>&1 && fail "an envelope with no key should be an error"
want 'key_keyowner="Other Vendor"'
if grep -qF inner_state log; then fail "the error names the protected text"; fi

# ---- an IEEE 1800 clause 34 envelope, as older tools write them -----------
# PKCS#1 v1.5 key_block, AES-128-CBC with the IV in the first block, no
# key_public_key, and the encoding directives those tools emit.
"$OPENSSL" rand -out sk.bin 16
"$OPENSSL" pkeyutl -encrypt -pubin -inkey acme_pub.pem -pkeyopt rsa_padding_mode:pkcs1 -in sk.bin -out kb1.bin
"$OPENSSL" base64 -in kb1.bin -out kb1.b64
"$OPENSSL" rand -out iv1.bin 16
"$OPENSSL" enc -aes-128-cbc -K "$(hex sk.bin)" -iv "$(hex iv1.bin)" -in "$T/interop_ip.sv" -out ct1.bin
cat iv1.bin ct1.bin > data1.bin
"$OPENSSL" base64 -in data1.bin -out db1.b64
{
  echo '`pragma protect begin_protected'
  echo '`pragma protect encrypt_agent="other_vendor_protect", encrypt_agent_info="3.1"'
  echo '`pragma protect key_keyowner="Acme", key_keyname="acme-1", key_method="rsa"'
  printf '`pragma protect encoding=(enctype="base64", line_length=64, bytes=%s)\n' "$(wc -c < kb1.bin)"
  echo '`pragma protect key_block'
  cat kb1.b64
  echo '`pragma protect data_method="aes128-cbc"'
  printf '`pragma protect encoding=(enctype="base64", line_length=64, bytes=%s)\n' "$(wc -c < data1.bin)"
  echo '`pragma protect data_block'
  cat db1.b64
  echo '`pragma protect end_protected'
} > v1.svp
"$MSIM" -keyfile Acme:acme-1=acme.pem v1.svp "$T/interop_tb.sv" > log 2>&1 || fail "the clause 34 envelope did not run"
want "tb: y=55"

echo "ok    msim opens IEEE 1735 and clause 34 envelopes it did not make"
