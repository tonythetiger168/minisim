#!/bin/sh
# The IEEE 1735-2023 clause 8 license system end to end: mlicproxy (the IP
# author's proxy) and msim (the tool) over TLS on the loopback interface.
#
#   license_proxy.sh <msim> <msimprotect> <mlicproxy> <openssl> <source dir> <work dir>
#
# Checks: a granted and a denied license call (8.4); license_string (8.8.2);
# the port@host list is tried in order and only a failed connection moves to
# the next pair (8.7); the proxy must hold the envelope's license_certificate
# (8.3.2); a garbled reply and a wrong request id (8.7.1); a proxy that never
# answers (8.7.2); the request-id-0 banner (8.8.3); return value reuse (8.6);
# irregularities (8.9); an expired feature and a license file changed while
# the proxy runs (8.5).
set -u
MSIM=$1; PROTECT=$2; PROXY=$3; OPENSSL=$4; SRC=$5; OUT=$6
EX=$SRC/examples/protect
rm -rf "$OUT"; mkdir -p "$OUT"; cd "$OUT" || exit 1
PIDS=""
trap 'kill $PIDS 2>/dev/null' EXIT
fail() { echo "FAIL: $*"; exit 1; }
has() { grep -q -- "$2" "$1" || { echo "--- $1"; cat "$1"; fail "expected '$2' in $1"; }; }
hasnt() { if grep -q -- "$2" "$1"; then echo "--- $1"; cat "$1"; fail "'$2' must not be in $1"; fi; }

# Keys: the tool's envelope key, and two proxy certificates.
$OPENSSL genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out acme.pem 2>/dev/null || fail genpkey
$OPENSSL pkey -in acme.pem -pubout -out acme_pub.pem || fail pubout
for n in proxy other; do
  $OPENSSL req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 30 -subj "/CN=$n" \
      -keyout ${n}_key.pem -out ${n}_cert.pem 2>/dev/null || fail "req $n"
done

# start <name> <cert> <license file> [mlicproxy options]: sets PORT_<name>.
start() {
  name=$1; cert=$2; lic=$3; shift 3
  rm -f $name.port
  "$PROXY" -cert ${cert}_cert.pem -key ${cert}_key.pem -bind 127.0.0.1 -licenses $lic -portfile $name.port -log "$@" 2>$name.log &
  PIDS="$PIDS $!"
  i=0
  while [ ! -s $name.port ]; do
    i=$((i + 1)); [ $i -gt 100 ] && { cat $name.log; fail "$name did not start"; }
    sleep 0.1
  done
  eval "PORT_$name=$(cat $name.port)"
}

cat > acme.lic <<LIC
# features of this site
acme_debug
acme_minver = "99.0"       # license_string value
acme_old expires 2001-01-01
LIC
start good proxy acme.lic -banner "Acme proxy 1.0"
start imposter other acme.lic
start garble proxy acme.lic -test_garble 1
start silent proxy acme.lic -test_silent
start wrongid proxy acme.lic -test_wrong_id

# ip <name> <control line>: secret_core with the proxy's common block and one
# right, as <name>/<name>.svp. Each IP has its own directory: msim opens every
# envelope it finds next to its sources (they may be `include files), and
# asks for their licenses too.
ip() {
  mkdir -p $1
  {
    echo '`pragma protect version=3'
    "$PROXY" -cert proxy_cert.pem -pragmas ACME_PROXY
    echo '`pragma protect begin_toolblock'
    echo '`pragma protect key_keyowner="Acme", key_keyname="acme-1"'
    echo '`pragma protect key_method="rsa_no_padding"'
    echo "\`pragma protect control $2"
    echo '`pragma protect end_toolblock'
    cat $EX/secret_core.sv
  } > $1.sv
  "$PROTECT" -key acme_pub.pem -keyowner Acme -keyname acme-1 $1.sv -o $1/$1.svp 2>$1.enc.log || { cat $1.enc.log; fail "msimprotect $1"; }
  has $1/$1.svp 'license_proxyenv'
  has $1/$1.svp 'license_certificate'
}
mkdir -p tb; cp $EX/core_tb.sv tb/
ip vis   'visibility = license("acme_debug") ? "full" : "none"'
ip novis 'visibility = license("acme_nope") ? "full" : "none"'
ip old   'visibility = license("acme_old") ? "full" : "none"'
ip minv  'min_version = license_string("acme_minver")'
ip late  'visibility = license("acme_late") ? "full" : "none"'

# sim <tag> <ip> [msim options]: runs msim, leaves <tag>.out/.err/.vcd and RC.
sim() {
  tag=$1; f=$2; shift 2
  "$MSIM" -keyfile Acme:acme-1=acme.pem "$@" $f/$f.svp tb/core_tb.sv -vcd $tag.vcd >$tag.out 2>$tag.err
  RC=$?
}

# 8.4: granted, with the banner (8.8.3) and the explanatory message shown.
export ACME_PROXY="$PORT_good@127.0.0.1"
sim t1 vis; [ $RC = 0 ] || { cat t1.err; fail "t1 rc=$RC"; }
has t1.vcd hidden_sum
has t1.err "license proxy: Acme proxy 1.0"
has t1.err "license proxy: License 'acme_debug' granted"
# 8.4: denied is an answer, not an irregularity.
sim t2 novis; [ $RC = 0 ] || fail "t2 rc=$RC"
hasnt t2.vcd hidden_sum
has t2.err "License 'acme_nope' denied"
hasnt t2.err "license check failed"
# 8.5: an expired feature.
sim t3 old; hasnt t3.vcd hidden_sum
has t3.err "License 'acme_old' expired on 2001-01-01"
# 8.8.2: license_string gives the string (here a min_version msim cannot meet).
sim t4 minv; [ $RC != 0 ] || fail "t4 should fail"
has t4.err "needs minisim 99.0 or later"

# 8.7: pairs are tried in order; a dead port, then a host that does not
# hold the certificate, then the real proxy. localhost as a host name.
export ACME_PROXY="1@127.0.0.1;$PORT_imposter@127.0.0.1;$PORT_good@localhost"
sim t5 vis; [ $RC = 0 ] || fail "t5 rc=$RC"
has t5.vcd hidden_sum
# 8.7: a secure connection that answers false ends the search: the second
# proxy would grant acme_late, and must not be asked.
echo "acme_late" > late.lic
start late proxy late.lic
export ACME_PROXY="$PORT_good@127.0.0.1;$PORT_late@127.0.0.1"
sim t6 late; hasnt t6.vcd hidden_sum
hasnt late.log acme_late
# 8.3.2: only an imposter answers.
export ACME_PROXY="$PORT_imposter@127.0.0.1"
sim t7 vis; [ $RC = 0 ] || fail "t7 rc=$RC"
hasnt t7.vcd hidden_sum
has t7.err "does not hold the envelope's license_certificate); licensed rights are denied"
# 8.9: the other allowed behaviour, stop.
sim t8 vis -license_fail stop; [ $RC != 0 ] || fail "t8 should fail"
has t8.err "license check failed"
# 8.9: the variable unset, empty, and malformed.
unset ACME_PROXY
sim t9 vis; hasnt t9.vcd hidden_sum; has t9.err "environment variable ACME_PROXY is not set"
export ACME_PROXY="27000 @host"
sim t10 vis; hasnt t10.vcd hidden_sum; has t10.err "is not a port@host"
export ACME_PROXY="127.0.0.1:27000"
sim t11 vis; hasnt t11.vcd hidden_sum; has t11.err "is not a port@host"

# 8.7.1: a garbled reply: disconnect, negotiate again, send the request again.
export ACME_PROXY="$PORT_garble@127.0.0.1"
sim t12 vis; [ $RC = 0 ] || fail "t12 rc=$RC"
has t12.vcd hidden_sum
[ "$(grep -c "acme_debug" garble.log)" = 2 ] || { cat garble.log; fail "the request was not sent again after a garbled reply"; }
# 8.7.1: a reply with a request id that was never sent is garbled too.
export ACME_PROXY="$PORT_wrongid@127.0.0.1"
sim t13 vis; hasnt t13.vcd hidden_sum; has t13.err "garbled reply"
# 8.7.2: no reply within the tool's timeout: next pair.
export ACME_PROXY="$PORT_silent@127.0.0.1;$PORT_good@127.0.0.1"
MINISIM_LICENSE_TIMEOUT=1 sim t14 vis; [ $RC = 0 ] || fail "t14 rc=$RC"
has t14.vcd hidden_sum
export ACME_PROXY="$PORT_silent@127.0.0.1"
MINISIM_LICENSE_TIMEOUT=1 sim t15 vis; hasnt t15.vcd hidden_sum; has t15.err "no reply from"

# 8.6 / 8.3: two envelopes with the same call in one run: one connection,
# one request.
export ACME_PROXY="$PORT_good@127.0.0.1"
sed 's/secret_core/secret_core2/g' vis.sv > vis2.sv
"$PROTECT" -key acme_pub.pem -keyowner Acme -keyname acme-1 vis2.sv -o vis/vis2.svp 2>/dev/null || fail "msimprotect vis2"
before=$(grep -c "acme_debug" good.log)
"$MSIM" -keyfile Acme:acme-1=acme.pem vis/vis.svp vis/vis2.svp tb/core_tb.sv -top core_tb >t16.out 2>t16.err || { cat t16.err; fail t16; }
after=$(grep -c "acme_debug" good.log)
[ $((after - before)) = 1 ] || fail "the same licensing call was sent $((after - before)) times in one run"

# 8.5: the license file changes while the proxy runs.
sim t17 late; hasnt t17.vcd hidden_sum
sleep 1.1; echo "acme_late" >> acme.lic
sim t18 late; has t18.vcd hidden_sum

# The other standard's spelling of the sequence identifier (X.690: 0x30).
MINISIM_LICENSE_SEQTAG=30 sim t19 vis; has t19.vcd hidden_sum

echo "license_proxy: all passed"
