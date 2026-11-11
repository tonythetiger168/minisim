#!/bin/sh
# IEEE 1735-2023 rules checked one by one: 5.2 (character set, case,
# end-of-file characters), Annex A (syntax), 9.3 and 9.4 (visibility in tool
# phases, child visibility, bound modules), 9.9 (dynamic objects).
#
#   protect_conformance.sh <msim> <msimprotect> <openssl> <source dir> <work dir>
set -u
MSIM=$1; PROTECT=$2; OPENSSL=$3; SRC=$4; OUT=$5
T=$SRC/tests/protect
rm -rf "$OUT"; mkdir -p "$OUT"; cd "$OUT" || exit 1

fail() { echo "FAIL: $*"; echo "--- output"; cat log; exit 1; }
want() { grep -qF -- "$1" log || fail "expected: $1"; }
wantnot() { grep -qF -- "$1" log && fail "must not appear: $1"; }
ok() { "$@" > log 2>&1 || fail "failed: $*"; }
bad() { "$@" > log 2>&1 && fail "expected a failure: $*"; }

"$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out k.pem 2>/dev/null || exit 1
"$OPENSSL" pkey -in k.pem -pubout -out k_pub.pem || exit 1
"$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out k2.pem 2>/dev/null || exit 1
enc() { "$PROTECT" -key k_pub.pem -keyowner Acme -keyname k1 "$@"; }

# ---- the reference: plain and protected runs print the same --------------
grep -v '`pragma protect' "$T/ip.sv" > ip_clear.sv
ok "$MSIM" ip_clear.sv "$T/tb.sv"
cp log plain.log
ok enc "$T/ip.sv" -o ip.svp
ok "$MSIM" -keyfile k.pem ip.svp "$T/tb.sv" -vcd w.vcd
# 9.2.2 / 9.3.2: protected names can be referenced from the user's HDL while
# the design is built, and the model behaves as in the clear.
cmp -s log plain.log || { diff plain.log log; fail "protected run differs from the plain run"; }
want "tb: hidden=21 open_a=0a open_b=0b"
# 9.9: a dynamic object made inside the IP is usable through the handle.
want "tb: token=42"
want "tb: q=23 fifo=2"

# ---- 9.3.3 / 9.4.2.2: what the waveform shows -----------------------------
cp w.vcd log
want '$var reg 8 & open_a $end'                       # viewport, Annex A list
grep -q ' open_b \$end' log || fail "viewport with a scoped name (ip_core:blk:open_b) is not visible"
wantnot ' hidden '
# A clear module instantiated inside the IP, and a module bound into it:
# the path to them goes through protected code, so they stay hidden.
wantnot 'kid'
wantnot 'kid_sig'
wantnot 'watched'
wantnot ' copy '
# 9.9: the class handle variables and the queue are protected names.
wantnot ' t $end'
wantnot ' make $end'
wantnot 'fifo'
# The lowering's temporaries may hold values computed inside the IP.
wantnot '_tf'

# ---- 5.2.1: strings ---------------------------------------------------------
printf '`pragma protect author = "a\\\\b"\n`pragma protect begin\nmodule m; endmodule\n`pragma protect end\n' > s1.sv
bad enc s1.sv -o s1.svp
want "printable ASCII without"
printf '`pragma protect author = "a\tb"\n`pragma protect begin\nmodule m; endmodule\n`pragma protect end\n' > s2.sv
bad enc s2.sv -o s2.svp
want "printable ASCII without"
printf '`pragma protect begin_toolblock\n`pragma protect key_keyowner = "Acme", key_keyname = "k1"\n`pragma protect control visibility = "fu\\\\ll"\n`pragma protect end_toolblock\n`pragma protect begin\nmodule m; endmodule\n`pragma protect end\n' > s3.sv
bad enc s3.sv -o s3.svp
want "5.2.1"

# ---- 5.2.2: case ------------------------------------------------------------
printf '`pragma protect BEGIN\nmodule m; endmodule\n`pragma protect END\n' > c1.sv
bad enc c1.sv -o c1.svp
want "keywords are lower case"
want "5.2.2"
printf '`pragma protect begin\nmodule m; endmodule\n`pragma protect End\n' > c2.sv
bad enc c2.sv -o c2.svp
want "keywords are lower case"
printf 'Acme key1 k.pem\nACME KEY1 k2.pem\n' > ring.txt
bad "$MSIM" -keyring ring.txt ip.svp "$T/tb.sv"
want "differ only in letter case"
printf '`pragma protect begin_toolblock\n`pragma protect key_keyowner = "Acme", key_keyname = "k1"\n`pragma protect control visibility = "full", Visibility = "none"\n`pragma protect end_toolblock\n`pragma protect begin\nmodule m; endmodule\n`pragma protect end\n' > c3.sv
bad enc c3.sv -o c3.svp
want "differ only in letter case"
# Standardized strings are matched with their case.
printf '`pragma protect data_method = "AES256-CBC"\n`pragma protect begin\nmodule m; endmodule\n`pragma protect end\n' > c4.sv
bad enc c4.sv -o c4.svp
# A keyword in the wrong case inside an envelope is refused too.
sed 's/`pragma protect key_block/`pragma protect KEY_BLOCK/' ip.svp > c5.svp
bad "$MSIM" -keyfile k.pem c5.svp "$T/tb.sv"
want "keywords are lower case"

# ---- 5.2.3: no end-of-file character inside an envelope -------------------
awk '{print} /begin_toolblock/ && !d {printf "%c\n", 5; d=1}' ip.svp > eof.svp
bad "$MSIM" -keyfile k.pem eof.svp "$T/tb.sv"
want "end-of-file character inside a decryption envelope"
want "5.2.3"

# ---- Annex A ----------------------------------------------------------------
# BASE64_TEXT has no spaces, and padding only at the end.
awk '/data_block/ {print; getline; print substr($0,1,10) " " substr($0,11); next} {print}' ip.svp > b1.svp
bad "$MSIM" -keyfile k.pem b1.svp "$T/tb.sv"
want "is not base64 text"
# A tool block holds key_keyowner, key_keyname, key_method, key_block and
# control; a common block only license_proxyenv and license_certificate.
sed 's/`pragma protect end_toolblock/`pragma protect author = "x"\n`pragma protect end_toolblock/' ip.svp > a1.svp
bad "$MSIM" -keyfile k.pem a1.svp "$T/tb.sv"
want "does not belong in a tool block"
sed 's/`pragma protect begin_toolblock/`pragma protect begin_commonblock\n`pragma protect author = "x"\n`pragma protect end_commonblock\n`pragma protect begin_toolblock/' ip.svp > a2.svp
bad "$MSIM" -keyfile k.pem a2.svp "$T/tb.sv"
want "only license_proxyenv and license_certificate belong in a common block"
# version is an INTEGER.
sed 's/`pragma protect version=3/`pragma protect version="3"/' ip.svp > a3.svp
bad "$MSIM" -keyfile k.pem a3.svp "$T/tb.sv"
wantnot "tb: hidden"
# The output of msimprotect stays inside the grammar: lower-case keywords,
# base64 lines of [A-Za-z0-9+/=], a quoted signature after end_protected.
awk '/begin_protected/ { in_env = 1 }
     !in_env { next }
     /^`pragma protect/ {
        k = $3; sub(/[ =,].*/, "", k);
        if (k != tolower(k)) { print "upper-case keyword: " $0; bad = 1 }
        if (k == "end_protected") in_env = 0
        next }
     /[^A-Za-z0-9+\/=]/ { print "not base64: " $0; bad = 1 }
     END { exit bad }' ip.svp > log 2>&1 || fail "msimprotect output outside Annex A"
grep '`pragma protect end_protected' ip.svp | grep -Eq '^`pragma protect end_protected="[A-Za-z0-9+/]+={0,2}"$' ||
    { grep end_protected ip.svp > log; fail "end_protected line"; }

echo "protect_conformance: ok"
