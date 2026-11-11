#!/bin/sh
# VPI and DPI with protected IP (IEEE 1800-2023 37.3.6 and clause 38;
# IEEE 1735-2023 9.7).
#
#   pli_protect.sh <msim> <msimprotect> <openssl> <source dir> <work dir>
#
# tests/pli/secret_pli.sv is encrypted and run with tests/pli/pli_tb.sv and
# the C code of tests/pli/probe.c. The IP's own system task, system function
# and DPI import must work as in the plain run; the testbench's $probe must
# learn nothing but vpiType and vpiIsProtected.
set -u
MSIM=$1; PROTECT=$2; OPENSSL=$3; SRC=$4; OUT=$5
P=$SRC/tests/pli
rm -rf "$OUT"; mkdir -p "$OUT"; cd "$OUT" || exit 1

fail() { echo "FAIL: $*"; echo "--- output"; cat log; exit 1; }
want() { grep -qF -- "$1" log || fail "expected: $1"; }
wantnot() { grep -qF -- "$1" log && fail "must not appear: $1"; }

"$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out acme.pem 2>/dev/null || exit 1
"$OPENSSL" pkey -in acme.pem -pubout -out acme_pub.pem || exit 1

# ---- plain: nothing is protected -----------------------------------------
"$MSIM" "$P/secret_pli.sv" "$P/pli_tb.sv" "$P/probe.c" > log 2>&1 || fail "plain run"
want "ip_task: call type=1 protected=0"
want "ip_task: arg0 protected=0 size=8 value=22"
want "probe: instance type=1 protected=0"
want "probe: instance name=u error=0"
want "probe: by_name pli_tb.u.hidden: handle error=0"
want "probe: kept name=hidden error=0"
want "probe: kept callback: handle error=0"
want "dpi 1: scope=pli_tb.u caller=secret_pli.sv:"
want "dpi lookup: found=1 name=pli_tb.u userdata=42"
want "dpi lookup: child name=pli_tb.u.c"

# ---- protected ------------------------------------------------------------
"$PROTECT" -key acme_pub.pem -keyowner Acme -keyname acme-1 "$P/secret_pli.sv" -o secret_pli.svp > plog 2>&1 || { cat plog; exit 1; }
grep -q hidden secret_pli.svp && { echo "FAIL: cleartext in the envelope"; exit 1; }
"$MSIM" -keyfile acme.pem secret_pli.svp "$P/pli_tb.sv" "$P/probe.c" > log 2>&1 || fail "protected run"

# 9.7.1 / 37.3.6: the IP's own call works (model fidelity). The call and its
# arguments are protected; vpiType, vpiIsProtected, the argument iteration
# (38.36) and vpiSize of an expression (37.57) are all there is, plus the
# values of the running call's own arguments.
want "ip_task: call type=1 protected=1"
want "ip_task: arg0 protected=1 size=8 value=22"
want "ip_task: arg1 protected=0 size=8 value=11"
want "ip_task: arg2 protected=1 size=8 value=5a"
want "ip_task: arg3 protected=1 size=8 value=2b"
want "ip_task: put error=0"
want "ip_task: scope of the call: null error=1"
# $ip_task wrote 33, $ip_func added one: the IP behaves as in the plain run.
want "hidden=34"

# The instance is reached by navigation, and says only its type.
want "probe: instance type=1 protected=1"
want "probe: instance name=(null) error=1"
want "probe: instance fullname=(null) error=1"
want "probe: instance defname=(null) error=1"
want "probe: instance type string=vpiModule error=0"
want "probe: instance line=-1 error=1"
want "probe: iterate nets: null error=1"
want "probe: iterate regs: null error=1"
want "probe: iterate ports: null error=1"
want "probe: iterate modules: null error=1"
want "probe: iterate parameters: null error=1"
want "probe: handle parent: null error=1"
want "probe: by_name hidden in the protected scope: null error=1"

# 38.20: names in protected code find nothing, disclosed or not; clear text
# below a protected instance is closed too (9.2: the path is opaque).
want "probe: by_name pli_tb.u: null error=1"
want "probe: by_name pli_tb.u.hidden: null error=1"
want "probe: by_name pli_tb.u.mem: null error=1"
want "probe: by_name pli_tb.u.SECRET_P: null error=1"
want "probe: by_name pli_tb.u.c: null error=1"
want "probe: by_name pli_tb.u.c.cc: null error=1"
want "probe: by_name pli_tb.u.q: null error=1"
# A viewport object is open; the user's own objects are untouched.
want "probe: viewport object pli_tb.u.seen protected=0 value=11"
want "probe: clear object d protected=0"

# A handle kept from the IP's call is of no use once the call is over.
want "probe: kept type=1 protected=1"
want "probe: kept name=(null) error=1"
want "probe: kept size=8 error=0"   # 37.57: vpiSize of a protected expression
want "probe: kept get_value error=1"
want "probe: kept put_value error=1"
want "probe: kept force error=1"
want "probe: kept bit: null error=1"
want "probe: kept bits: null error=1"
want "probe: kept scope: null error=1"
want "probe: kept callback: null error=1"
want "VPI error: vpi_register_cb: a callback cannot be placed on a protected object"   # 38.36: with a message
want "probe: kept expression protected=1 size=8 get_value error=1"
wantnot "a callback fired on a protected object"
want "ip: hidden=34 after the probe"

# 9.7.2.3 DPI: no scope name and no caller position from protected code;
# svGetScopeFromName with the disclosed name still gives a usable context.
want "dpi 1: scope=<protected> caller=<protected>:0"
want "dpi 2: scope=pli_tb caller=pli_tb.sv:"
want "dpi lookup: found=1 name=<protected> userdata=42"
want "dpi lookup: child name=<protected>"
want "dpi lookup: own name=pli_tb"

# No position in the IP author's source in the output.
wantnot "secret_pli.sv"
echo "pli_protect: ok"
