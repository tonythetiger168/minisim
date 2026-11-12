#!/bin/bash
# mvplat.sh - the virtual platform's own tests: programs on the built-in board
# and on platform files, interrupts, PMP, several harts on several threads
# (the result must not depend on the thread count), the commit log, and the
# errors a platform file or the command line can make.
#   tests/mvplat.sh MVPLAT SRC OUT
set -u
MVPLAT=$1 SRC=$2 OUT=$3
T=$SRC/tests/vplat
E=$T/elf
rm -rf "$OUT"; mkdir -p "$OUT"; cd "$OUT"
fail=0
ok()  { echo "ok    $1"; }
bad() { echo "FAIL  $1"; fail=1; }
# expect NAME CODE FILE PATTERN: exit code and a line in the output
expect() {
  local name=$1 code=$2 file=$3 pat=$4
  if [ "$rc" != "$code" ]; then bad "$name: exit $rc, expected $code"; sed 's/^/      /' "$file"; return; fi
  if ! grep -q -- "$pat" "$file"; then bad "$name: no '$pat' in the output"; sed 's/^/      /' "$file"; return; fi
  ok "$name"
}

"$MVPLAT" "$E/hello.elf" > hello.out 2>&1; rc=$?
expect "hello on the built-in board" 0 hello.out "hello from mvplat"

"$MVPLAT" "$E/hello.elf" -map -q > map.out 2>&1; rc=$?
expect "-map" 0 map.out "0x80000000..0x87ffffff  ram        dram"

"$MVPLAT" "$E/irq.elf" -input abc -q > irq.out 2>&1; rc=$?
expect "timer, software and external interrupts" 0 irq.out "timer ticks 5, software 1, received 'abc'"

"$MVPLAT" "$E/pmp.elf" -q > pmp.out 2>&1; rc=$?
expect "PMP: NAPOT, NA4, TOR, partial match, MPRV, locked entries" 0 pmp.out "pmp: all checks passed"

# Several harts: the commit log and the console are the same on
# one thread and on four.
for t in 1 4; do
  "$MVPLAT" "$E/smp.elf" -harts 4 -threads $t -q -trace smp$t.log > smp$t.out 2>&1; rc=$?
  expect "4 harts on $t thread(s): atomics and LR/SC" 0 smp$t.out "harts 4 atomic 8000 locked 8000"
done
cmp -s smp1.out smp4.out && cmp -s smp1.log smp4.log \
  && ok "1 thread and 4 threads give the same run, instruction for instruction" \
  || bad "the run depends on the thread count"
for t in 1 3; do
  "$MVPLAT" "$E/bench.elf" -harts 3 -threads $t -q > bench$t.out 2>&1; rc=$?
  expect "compute on 3 harts, $t thread(s)" 0 bench$t.out "hart 2 "
done
cmp -s bench1.out bench3.out && ok "bench: the same on 1 and 3 threads" || bad "bench depends on the thread count"

"$MVPLAT" "$E/irq.elf" -input abc -q -max-cycles 20000 > part1.out 2>&1; rc=$?
expect "-max-cycles stops with exit code 4" 4 part1.out "stopped at cycle 20000"

# The commit log, in spike's format.
"$MVPLAT" "$E/hello.elf" -q -trace hello.log > /dev/null 2>&1
head -1 hello.log | grep -q "^core   0: 3 0x80000000 (0x" && ok "-trace: spike-style commit log" || { bad "-trace"; head -3 hello.log; }

# Options and devices of the extension library, when none is installed.
if [ -z "${MVPLAT_EXT:-}" ]; then
  "$MVPLAT" "$E/hello.elf" -gdb 0 > p.out 2>&1; rc=$?
  expect "-gdb without the extension library" 2 p.out "needs the mvplat extension library"
  printf '%s\n' '{"devices": [{"type":"ram","base":"0x80000000","size":"1M"},{"type":"rtl","base":"0x40000000","size":"4K"}]}' > p.json
  "$MVPLAT" -platform p.json "$E/hello.elf" -q > p.out 2>&1; rc=$?
  expect "an rtl device without the extension library" 2 p.out "mvp and rtl need the mvplat extension library"
fi

# Platform files that are wrong say what is wrong.
platform_err() {
  local name=$1 json=$2 pat=$3
  printf '%s\n' "$json" > p.json
  "$MVPLAT" -platform p.json "$E/hello.elf" -q > p.out 2>&1; rc=$?
  expect "$name" 2 p.out "$pat"
}
platform_err "overlapping devices" '{"devices": [{"type":"ram","name":"a","base":"0x80000000","size":"1M"},{"type":"ram","name":"b","base":"0x80080000","size":"1M"}]}' "overlaps 'a'"
platform_err "unknown device type" '{"devices": [{"type":"gpu","base":0}]}' "unknown device type 'gpu'"
platform_err "JSON syntax" '{"devices": [}' "line 1"
platform_err "no memory for the program" '{"devices": [{"type":"ram","base":"0x1000","size":"4K"}]}' "no memory at 0x80000000"
printf '%s\n' '{"harts": 2, "devices": [{"type":"ram","base":"0x80000000","size":"0x100800"}]}' > p.json
"$MVPLAT" -platform p.json "$E/hello.elf" -q > p.out 2>&1; rc=$?
expect "several harts need page-aligned memory" 2 p.out "4 KiB boundary"
"$MVPLAT" "$E/hello.elf" -no_such_option > p.out 2>&1; rc=$?
expect "an unknown option" 2 p.out "unknown option"

# A program that waits for an interrupt nobody will send ends rather than hangs.
cat > wfi.json <<'J'
{"devices": [{"type":"ram","base":"0x80000000","size":"1M"}]}
J
"$MVPLAT" -platform wfi.json "$E/irq.elf" -q -max-cycles 100000000 > wfi.out 2>&1; rc=$?
[ $rc -ne 0 ] && ok "a program with no CLINT does not hang (exit $rc)" || bad "wfi with no interrupt source"

exit $fail
