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
# Input later on: at a simulated time (the harts wait in WFI until then),
# and from standard input.
"$MVPLAT" "$E/irq.elf" -input-at "0.002:ab" -input-at "0.003:c" -q > irqat.out 2>&1; rc=$?
expect "-input-at: text at a simulated time" 0 irqat.out "received 'abc'"
printf abc | "$MVPLAT" "$E/irq.elf" -stdin -q > irqin.out 2>&1; rc=$?
expect "-stdin: what standard input brings" 0 irqin.out "received 'abc'"
"$MVPLAT" "$E/irq.elf" -input-at "soon:abc" -q > irqbad.out 2>&1; rc=$?
expect "-input-at refuses a time that is not a number" 2 irqbad.out "SECONDS:TEXT"

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

# RV64: the same programs built as 64-bit ELF files run on 64-bit harts.
"$MVPLAT" "$E/hello64.elf" -q > hello64.out 2>&1; rc=$?
expect "RV64: hello" 0 hello64.out "hello from mvplat"
"$MVPLAT" "$E/irq64.elf" -input abc -q > irq64.out 2>&1; rc=$?
expect "RV64: timer, software and external interrupts" 0 irq64.out "timer ticks 5, software 1, received 'abc'"
for t in 1 4; do
  "$MVPLAT" "$E/smp64.elf" -harts 4 -threads $t -q -trace smp64_$t.log > smp64_$t.out 2>&1; rc=$?
  expect "RV64: 4 harts on $t thread(s)" 0 smp64_$t.out "harts 4 atomic 8000 locked 8000"
done
cmp -s smp64_1.log smp64_4.log && ok "RV64: the same run on 1 and 4 threads" || bad "RV64: the run depends on the thread count"
head -1 smp64_1.log | grep -q "^core   0: 3 0x0000000080000000 (0x" && ok "RV64: 64-bit commit log" || { bad "RV64 -trace"; head -3 smp64_1.log; }
# -serial: the harts take turns on one thread, storing to memory directly.
for e in smp smp64; do
  for o in "" "-quantum 7" -nojit; do
    "$MVPLAT" "$E/$e.elf" -harts 4 -serial -q $o > ser.out 2>&1; rc=$?
    expect "-serial $e $o: atomics and LR/SC" 0 ser.out "harts 4 atomic 8000 locked 8000"
  done
done
"$MVPLAT" "$E/smp64.elf" -harts 4 -serial -q -trace ser1.log > /dev/null 2>&1
"$MVPLAT" "$E/smp64.elf" -harts 4 -serial -q -trace ser2.log > /dev/null 2>&1
cmp -s ser1.log ser2.log && ok "-serial: the same run twice" || bad "-serial: two runs differ"
"$MVPLAT" "$E/smp64.elf" -harts 4 -q -trace ser3.log > /dev/null 2>&1
cmp -s ser1.log ser3.log && ok "several harts take turns by default (the same as -serial)" || bad "the default with several harts is not -serial"
# -mt: each hart on a thread of its own, at once; AMOs and LR/SC as host
# atomics (smpmt64 waits for all four harts: they may start far apart).
for o in "" "-threads 2" -nojit "-quantum 1000"; do
  for i in 1 2 3; do
    "$MVPLAT" "$E/smpmt64.elf" -harts 4 -mt -q $o > mt.out 2>&1; rc=$?
    grep -q "harts 4 atomic 8000 locked 8000" mt.out && [ $rc = 0 ] || break
  done
  expect "-mt $o: atomics and LR/SC across threads (3 runs)" 0 mt.out "harts 4 atomic 8000 locked 8000"
done

# The V extension: clang's vectorized loops agree with its scalar ones
# (RV32 and RV64, at the smallest and largest VLEN), and single
# instructions agree with scalar models of the specification.
for x in 32 64; do
  for v in 128 1024; do
    "$MVPLAT" "$E/vec$x.elf" -vlen $v -q > vec${x}_$v.out 2>&1; rc=$?
    expect "V, RV$x, VLEN $v: vectorized loops agree with scalar ones" 0 vec${x}_$v.out "17 of 17 kernels agree"
    "$MVPLAT" "$E/vec${x}m8.elf" -vlen $v -q > vec${x}m8_$v.out 2>&1; rc=$?
    expect "V, RV$x, VLEN $v: the same with LMUL 8 groups" 0 vec${x}m8_$v.out "17 of 17 kernels agree"
  done
  for v in 128 512; do
    "$MVPLAT" "$E/vops$x.elf" -vlen $v -q > vops${x}_$v.out 2>&1; rc=$?
    expect "V, RV$x, VLEN $v: single instructions, traps, vstart" 0 vops${x}_$v.out "vops: all checks passed"
    "$MVPLAT" "$E/zvbb$x.elf" -vlen $v -q > zvbb${x}_$v.out 2>&1; rc=$?
    expect "Zvbb, RV$x, VLEN $v: every instruction at SEW 8 to 64" 0 zvbb${x}_$v.out "checks passed, all"
    "$MVPLAT" "$E/zvfh$x.elf" -vlen $v -q > zvfh${x}_$v.out 2>&1; rc=$?
    expect "Zvfh, RV$x, VLEN $v: half precision against Zfh, five rounding modes" 0 zvfh${x}_$v.out "checks passed, all"
    "$MVPLAT" "$E/bf16$x.elf" -vlen $v -q > bf16${x}_$v.out 2>&1; rc=$?
    expect "Zfbfmin/Zvfbfmin/Zvfbfwma, RV$x, VLEN $v: BFloat16 conversions and widening multiply-add" 0 bf16${x}_$v.out "checks passed, all"
    "$MVPLAT" "$E/zvk$x.elf" -vlen $v -q > zvk${x}_$v.out 2>&1; rc=$?
    expect "Zvkned/Zvknhb/Zvkg/Zvbc, RV$x, VLEN $v: AES, SHA-2, GHASH known answers" 0 zvk${x}_$v.out "checks passed, all"
  done
done
"$MVPLAT" "$E/jit64.elf" -q > jit.out 2>&1; rc=$?
expect "translator, RV64: M-extension corner cases, translated; machine mode and supervisor mode with Sv39" 0 jit.out "checks passed, all"
"$MVPLAT" "$E/jit64.elf" -q -nojit > jit_nojit.out 2>&1
"$MVPLAT" "$E/jit64.elf" -q -quantum 7 > jit_q7.out 2>&1
"$MVPLAT" "$E/jit64.elf" > jit_count.out 2>&1
"$MVPLAT" "$E/jit64.elf" -nojit > jit_count_nojit.out 2>&1
cmp -s jit.out jit_nojit.out && cmp -s jit.out jit_q7.out && \
  [ "$(grep -o '[0-9]* instructions' jit_count.out)" = "$(grep -o '[0-9]* instructions' jit_count_nojit.out)" ] \
  && ok "translator: the same checksum and instruction count as -nojit, and with a 7-cycle quantum" \
  || bad "translator: differs from -nojit"
for o in "" -nojit "-quantum 7" "-harts 2" "-harts 2 -parallel"; do
  "$MVPLAT" "$E/smc64.elf" -q $o > smc.out 2>&1; rc=$?
  expect "self-modifying code and FENCE.I, machine and supervisor mode (Sv39, an alias) ${o:-(translated)}" 0 smc.out "checks passed, all"
done
# the same with code pages compared at every FENCE.I instead of watched
MVPLAT_NO_WTRACK=1 "$MVPLAT" "$E/smc64.elf" -q > smc.out 2>&1; rc=$?
expect "self-modifying code, pages not watched" 0 smc.out "checks passed, all"
"$MVPLAT" "$E/svpbmt64.elf" -q > svpbmt.out 2>&1; rc=$?
expect "Svpbmt (PBMT, menvcfg.PBMTE) and Zalasr (load-acquire, store-release)" 0 svpbmt.out "checks passed, all"

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

# The devices an Android guest needs beyond Linux's: goldfish-rtc (time,
# setting it, an alarm and its interrupt) and virtio-rng (the same bytes for
# the same seed).
"$MVPLAT" -platform "$T/androiddev.json" "$E/androiddev.elf" -q > adev.out 2>&1; rc=$?
expect "goldfish-rtc and virtio-rng" 0 adev.out "android devices: all checks passed"
"$MVPLAT" -platform "$T/androiddev.json" "$E/androiddev.elf" -q > adev2.out 2>&1
cmp -s adev.out adev2.out && ok "virtio-rng gives the same bytes for the same seed" || bad "virtio-rng differs between runs"

# A PCIe host (ECAM) with virtio devices behind it: enumeration, BAR
# sizing, the virtio PCI capabilities, a request, and INTx shared by two
# devices; the device tree describes the host as QEMU's virt board does.
"$MVPLAT" -platform "$T/pcie.json" "$E/pcie.elf" -q > pcie.out 2>&1; rc=$?
expect "PCIe host and virtio over PCI" 0 pcie.out "pcie: all checks passed"
"$MVPLAT" -platform "$T/pcie.json" -dump-dtb pcie.dtb "$E/pcie.elf" -q > /dev/null 2>&1
grep -q "pci-host-ecam-generic" pcie.dtb && grep -q "interrupt-map" pcie.dtb &&
  ok "the device tree has the ECAM host and its interrupt-map" || bad "PCIe host in the device tree"

# virtio-gpu: a bare-metal driver draws through the 2D commands; the
# picture the device writes at the end ("screen") is what it drew.
sed 's/"height": 32}/"height": 32, "screen": "gpu.ppm"}/' "$T/gpu.json" > gpu.json
rm -f gpu.ppm
"$MVPLAT" -platform gpu.json "$E/gpu.elf" -q > gpu.out 2>&1; rc=$?
expect "virtio-gpu: display info, resources, transfers, scanout, flush" 0 gpu.out "gpu: all checks passed"
if command -v python3 > /dev/null 2>&1; then
  python3 - <<'P' && ok "virtio-gpu: the screen file holds the picture" || bad "virtio-gpu screen file"
import sys
b = open('gpu.ppm', 'rb').read()
h = b'P6\n64 32\n255\n'
assert b[:len(h)] == h and len(b) == len(h) + 64 * 32 * 3, b[:20]
p = b[len(h):]
for y in range(32):
    for x in range(64):
        want = (0x80, 8 * y, 4 * x)
        if 10 <= x < 14 and 5 <= y < 8: want = (255, 255, 255)
        got = tuple(p[3 * (y * 64 + x):3 * (y * 64 + x) + 3])
        assert got == want, (x, y, got, want)
P
fi

# "frames": the screen every frame_ms of simulated time while it changes.
sed 's/"height": 32}/"height": 32, "frames": "fr\/s", "frame_ms": 1}/' "$T/gpu.json" > gpuf.json
rm -rf fr; mkdir fr
"$MVPLAT" -platform gpuf.json "$E/gpu.elf" -q > gpuf.out 2>&1; rc=$?
expect "virtio-gpu frames: the run" 0 gpuf.out "gpu: all checks passed"
nf=$(ls fr | grep -c '^s-[0-9]\{9\}\.ppm$')
[ "$nf" -ge 1 ] && cmp -s "fr/$(ls fr | tail -1)" gpu.ppm && ok "virtio-gpu frames: $nf frame(s), the last one the final picture" \
  || { bad "virtio-gpu frames"; ls fr; }

# The Android recipe's image tools (tests/vplat/android), on small made-up
# images: the initrd is vendor ramdisk + ramdisk + a bootconfig trailer, and
# the disk is a GPT with the images at their partitions.
if command -v python3 > /dev/null; then
  mkdir -p aimg
  python3 - <<'P' || bad "android: made-up boot images"
import struct
def page(b): return b + b'\0' * (-len(b) % 4096)
# init_boot: a v4 boot image with no kernel and a 5-byte ramdisk.
h = b'ANDROID!' + struct.pack('<4I', 0, 5, 0, 4096) + b'\0' * 20 + struct.pack('<I', 4)
open('aimg/init_boot.img', 'wb').write(page(h) + page(b'RAMDK'))
# vendor_boot v4: a 3-byte vendor ramdisk, no dtb, no table, a bootconfig.
bc = b'androidboot.hardware=cutf_cvm\nandroidboot.slot_suffix=_b\n'
h = b'VNDRBOOT' + struct.pack('<5I', 4, 4096, 0, 0, 3) + b'\0' * 2048 + b'\0' * 20
h += struct.pack('<II', 2128, 0) + struct.pack('<Q', 0) + struct.pack('<4I', 0, 0, 0, len(bc))
open('aimg/vendor_boot.img', 'wb').write(page(h) + page(b'VRD') + page(bc))
for n in ['boot', 'vbmeta', 'vbmeta_system', 'vbmeta_system_dlkm', 'vbmeta_vendor_dlkm', 'userdata']:
    open('aimg/%s.img' % n, 'wb').write(n.encode())
# super as an Android sparse image: 1 raw block, 1 fill block.
blk = 4096
sp = struct.pack('<IHHHHIIII', 0xed26ff3a, 1, 0, 28, 12, blk, 2, 2, 0)
sp += struct.pack('<HHII', 0xcac1, 0, 1, 12 + blk) + b'S' * blk
sp += struct.pack('<HHII', 0xcac2, 0, 1, 16) + b'\x01\x02\x03\x04'
open('aimg/super.img', 'wb').write(sp)
P
  python3 -I "$T/android/mkinitrd.py" a.initrd aimg/init_boot.img aimg/vendor_boot.img androidboot.lcd_density=240 > /dev/null &&
  python3 -I "$T/android/mkinitrd.py" a.initrd - - androidboot.serialconsole=0 > /dev/null &&
  python3 -I "$T/android/mkdisk.py" a.disk aimg > /dev/null &&
  python3 - <<'P' && ok "android: initrd and GPT disk from boot images" || bad "android image tools"
import struct, zlib
d = open('a.initrd', 'rb').read()
assert d.endswith(b'#BOOTCONFIG\n')
size, csum = struct.unpack('<II', d[-20:-12])
body = d[-20 - size:-20]
assert sum(body) & 0xffffffff == csum
assert d[:-20 - size] == b'VRD\0RAMDK\0\0\0', d[:16]
kv = dict(l.split('=', 1) for l in body.rstrip(b'\0').decode().split('\n') if l)
assert kv['androidboot.hardware'] == 'cutf_cvm'
assert kv['androidboot.slot_suffix'] == '_a'          # the recipe's entry wins
assert kv['androidboot.lcd_density'] == '240'         # then the arguments
assert kv['androidboot.serialconsole'] == '0'         # a rewrite keeps the rest
assert kv['androidboot.hw_timeout_multiplier'] == '20'
g = open('a.disk', 'rb').read()
assert g[510:512] == b'\x55\xaa' and g[512:520] == b'EFI PART'
hdr = g[512:512 + 92]
assert zlib.crc32(hdr[:16] + b'\0' * 4 + hdr[20:]) & 0xffffffff == struct.unpack('<I', hdr[16:20])[0]
ents = g[1024:1024 + 128 * 128]
assert zlib.crc32(ents) & 0xffffffff == struct.unpack('<I', hdr[88:92])[0]
parts = {}
for i in range(128):
    e = ents[128 * i:128 * i + 128]
    if e[:16] == b'\0' * 16: continue
    first, last = struct.unpack('<QQ', e[32:48])
    parts[e[56:].decode('utf-16-le').rstrip('\0')] = (first, last)
assert list(parts)[:3] == ['misc', 'frp', 'boot_a'] and 'super' in parts and 'userdata' in parts, parts
s = parts['super'][0] * 512
assert g[s:s + 4096] == b'S' * 4096 and g[s + 4096:s + 8192] == b'\x01\x02\x03\x04' * 1024
b = parts['boot_a'][0] * 512
assert g[b:b + 4] == b'boot'
alt = struct.unpack('<Q', hdr[32:40])[0]
assert g[alt * 512:alt * 512 + 8] == b'EFI PART'
P
fi
# cfhost.py, the host end of an Android guest's console ports: the sensors
# HAL's list-sensors and the OEM lock HAL's reads and writes.
mkdir -p cf
python3 -I "$T/android/cfhost.py" "$PWD/cf" > cfhost.out 2>&1 &
cfpid=$!
for i in $(seq 50); do [ -S cf/hvc19.sock ] && break; sleep 0.1; done
python3 - "$PWD/cf" <<'P' && ok "android: cfhost.py answers the sensors and OEM lock HALs" || bad "android: cfhost.py"
import socket, struct, sys
def talk(port, cmd, payload):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect('%s/hvc%d.sock' % (sys.argv[1], port))
    s.settimeout(10)
    s.sendall(struct.pack('<II', cmd, len(payload)) + payload)
    w0, n = struct.unpack('<II', s.recv(8))
    r = s.recv(n) if n else b''
    s.close()
    return w0, r
assert talk(18, 0, b'list-sensors') == (1 << 31, b'0')
assert talk(10, 1, b'\x01') == (1 << 31 | 1, b'\x01')
assert talk(10, 1, b'') == (1 << 31 | 1, b'\x01')
assert talk(10, 3, b'') == (1 << 31 | 3, b'\x00')
P
kill $cfpid 2> /dev/null; wait $cfpid 2> /dev/null

# virtio-blk (-disk): a bare-metal driver reads, writes and flushes a raw
# image; the file changes only with -disk-write.
if command -v python3 > /dev/null 2>&1; then
  python3 - <<'P'
d = bytearray(64 * 512)
for s in range(64):
    d[s * 512:(s + 1) * 512] = bytes([s]) * 512
d[0:11] = b"MVPLAT-DISK"
open("disk.img", "wb").write(d)
open("disk0.img", "wb").write(d)
P
  for e in virtio virtio64; do
    "$MVPLAT" "$E/$e.elf" -disk disk.img -q > $e.out 2>&1; rc=$?
    expect "$e: virtio-blk read, write, flush, errors and interrupts" 0 $e.out "virtio-blk: all checks passed"
  done
  cmp -s disk.img disk0.img && ok "-disk keeps the guest's writes out of the image" || bad "-disk changed the image"
  "$MVPLAT" "$E/virtio.elf" -disk disk.img -disk-write -q > vw.out 2>&1; rc=$?
  if [ $rc = 0 ] && [ "$(dd if=disk.img bs=512 skip=2 count=1 2> /dev/null | head -c 20)" = "written by the guest" ]; then
    ok "-disk-write writes to the image"
  else bad "-disk-write (exit $rc)"; fi
  printf '%s\n' '{"devices": [{"type":"finisher","base":"0x100000"},{"type":"uart16550","base":"0x10000000"},{"type":"ram","base":"0x80000000","size":"1M"},{"type":"virtio-blk","name":"vd","base":"0x10001000","irq":1,"file":"disk0.img","readonly":true}]}' > vd.json
  "$MVPLAT" -platform vd.json "$E/virtio.elf" -q -max-cycles 10000000 > vd.out 2>&1; rc=$?
  if [ $rc != 0 ] && grep -q "FAIL write sector 2" vd.out && ! grep -q "FAIL read sector 0" vd.out; then
    ok "a read-only virtio-blk from the platform file reads and refuses writes"
  else bad "read-only virtio-blk (exit $rc)"; sed 's/^/      /' vd.out; fi
fi
"$MVPLAT" "$E/virtio.elf" -disk no_such.img -q > p.out 2>&1; rc=$?
expect "-disk with a missing image" 2 p.out "cannot open the disk image"

# virtio-net (-net): a bare-metal driver asks the built-in gateway for its
# MAC address and pings it; -net-pcap records the four frames. Over -net
# dgram a peer on the host sends the frame back.
"$MVPLAT" "$E/vnet.elf" -net user -net-pcap vnet.pcap -q > vnet.out 2>&1; rc=$?
expect "virtio-net: ARP and ping to the built-in gateway" 0 vnet.out "vnet: gateway answered ARP and ping"
if command -v python3 > /dev/null 2>&1; then
  n=$(python3 -c "
import struct, sys
d = open('vnet.pcap', 'rb').read()
assert struct.unpack_from('<I', d)[0] == 0xa1b2c3d4
o, n = 24, 0
while o < len(d):
    o += 16 + struct.unpack_from('<I', d, o + 8)[0]; n += 1
print(n)" 2> /dev/null)
  [ "$n" = 4 ] && ok "-net-pcap records the frames both ways" || bad "-net-pcap: $n frames, expected 4"
  rm -f peer.sock vm.sock
  python3 "$T/echo_peer.py" "$PWD/peer.sock" "$PWD/vm.sock" > peer.out 2>&1 &
  for i in $(seq 50); do [ -S peer.sock ] && break; sleep 0.1; done
  "$MVPLAT" "$E/vnet.elf" -net "dgram:$PWD/vm.sock,$PWD/peer.sock" -q > dgram.out 2>&1; rc=$?
  wait
  expect "virtio-net over -net dgram to a peer on the host" 0 dgram.out "vnet: echo peer ok"
fi
# virtio-console (-vcon): a bare-metal driver prints through it, and -input
# arrives once the console has been quiet.
"$MVPLAT" "$E/vcon.elf" -vcon -q -input "typed line
" > vcon.out 2>&1; rc=$?
expect "virtio-console: output, and -input once the console is quiet" 0 vcon.out "^vcon: got typed line"
grep -q "^vcon: hello over virtio-console" vcon.out && ok "virtio-console output reaches the console" || bad "virtio-console output"
# virtio-console "socket": the console is a Unix stream socket a host
# program listens on; "output": its output goes to a file instead.
vcon_json() {
  cat > vs.json <<EOF
{"name": "virt32+console", "devices": [
  {"type": "finisher", "base": "0x00100000"}, {"type": "clint", "base": "0x02000000"},
  {"type": "plic", "base": "0x0c000000", "sources": 31},
  {"type": "uart16550", "name": "uart0", "base": "0x10000000", "irq": 10},
  {"type": "virtio-console", "name": "vcon", "base": "0x10001000", "irq": 1, $1},
  {"type": "ram", "name": "dram", "base": "0x80000000", "size": "16M"}]}
EOF
}
vcon_json '"socket": "vcon.sock"'
rm -f vcon.sock
python3 "$T/vcon_peer.py" "$PWD/vcon.sock" > vpeer.out 2>&1 &
for i in $(seq 50); do [ -S vcon.sock ] && break; sleep 0.1; done
"$MVPLAT" -platform vs.json "$E/vcon.elf" -q > vsock.out 2>&1; rc=$?
wait
[ "$rc" = 0 ] && ok "virtio-console socket: the guest runs to the end" || bad "virtio-console socket: exit $rc"
grep -q "^vcon: got line from the host" vpeer.out && ok "virtio-console socket: the host reads the output and types the input" || bad "virtio-console socket: $(head -c 300 vpeer.out)"
vcon_json '"output": "vcon.txt", "input": "typed\n"'
"$MVPLAT" -platform vs.json "$E/vcon.elf" -q > vfile.out 2>&1; rc=$?
grep -q "^vcon: got typed" vcon.txt && ! grep -q "^vcon:" vfile.out && ok "virtio-console output to a file" || bad "virtio-console output to a file (rc $rc)"
"$MVPLAT" "$E/vnet.elf" -net tap0 -q > p.out 2>&1; rc=$?
expect "-net with an unknown back end" 2 p.out "unknown back end 'tap0'"

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

# The device tree made from the platform, as firmware and Linux read it.
"$MVPLAT" "$E/hello.elf" -harts 2 -disk "$E/hello.elf" -net user -vcon -dump-dtb board.dtb > dtb.out 2>&1; rc=$?
if [ $rc = 0 ] && [ "$(od -An -tx1 -N4 board.dtb | tr -d ' ')" = d00dfeed ]; then ok "-dump-dtb writes a device tree blob"
else bad "-dump-dtb (exit $rc)"; fi

# AArch64 (tests/vplat/a64, built by its build.sh): the built-in arm board
# is chosen from the ELF's machine.
A=$T/a64/elf
"$MVPLAT" "$A/hello.elf" -input xyz -q > a64hello.out 2>&1; rc=$?
expect "aarch64: hello and PL011 input" 0 a64hello.out "received 'xyz'"
"$MVPLAT" "$A/hello.elf" -map -q > a64map.out 2>&1; rc=$?
expect "aarch64: -map shows the arm board" 0 a64map.out "0x080a0000..0x080bffff  gicv3-redist"
"$MVPLAT" "$A/calc.elf" -q > a64calc.out 2>&1; rc=$?
if [ $rc = 0 ] && diff -q a64calc.out "$T/a64/calc.expected" > /dev/null; then ok "aarch64: integer, floating-point and NEON results match the host's"
else bad "aarch64: calc (exit $rc)"; diff a64calc.out "$T/a64/calc.expected" | sed 's/^/      /'; fi
"$MVPLAT" "$A/trap.elf" -q > a64trap.out 2>&1; rc=$?
expect "aarch64: SVC, undefined, aborts, EL0, PSCI" 0 a64trap.out "trap: all passed"
"$MVPLAT" "$A/mmu.elf" -q > a64mmu.out 2>&1; rc=$?
expect "aarch64: stage 1 translation, faults, AT, TLBI" 0 a64mmu.out "mmu: all passed"
"$MVPLAT" "$A/irq.elf" -q > a64irq.out 2>&1; rc=$?
expect "aarch64: GICv3, timers, SGI, priority mask" 0 a64irq.out "irq: all passed"
"$MVPLAT" "$A/smp.elf" -harts 4 -q > a64smp.out 2>&1; rc=$?
expect "aarch64: PSCI CPU_ON, exclusives and a spin lock on 4 processors" 0 a64smp.out "smp: all passed"
"$MVPLAT" "$A/smc.elf" -q > a64smc.out 2>&1; rc=$?
expect "aarch64: code rewritten while it runs; the decoded load and store forms" 0 a64smc.out "smc: all passed"
"$MVPLAT" "$A/smpmt.elf" -harts 4 -q > a64smpmt.out 2>&1; rc=$?
expect "aarch64: shared counters by LDXR/STXR, LDADD, CAS, CASP and a lock on 4 processors" 0 a64smpmt.out "smpmt: all passed"
"$MVPLAT" "$A/smpmt.elf" -harts 4 -mt -q > a64smpmt2.out 2>&1; rc=$?
expect "aarch64: the same with -mt (4 host threads, the host's atomics)" 0 a64smpmt2.out "smpmt: all passed"
"$MVPLAT" "$A/smp.elf" -harts 4 -mt -q > a64smp2.out 2>&1; rc=$?
expect "aarch64: PSCI, exclusives and WFE/SEV with -mt" 0 a64smp2.out "smp: all passed"
"$MVPLAT" "$A/lse.elf" -q > a64lse.out 2>&1; rc=$?
expect "aarch64: Armv8.1 atomics (LDADD .. SWP, CAS, CASP) and CRC32" 0 a64lse.out "lse: all passed"
# Every block translated to host code at its first run (MVPLAT_JIT_HOT=1):
# the same results and the same instruction counts as with -nojit.
jitok=1
for t in calc trap mmu irq smc lse; do
  MVPLAT_JIT_HOT=1 "$MVPLAT" "$A/$t.elf" > a64jit-$t.out 2>&1 || jitok=0
  "$MVPLAT" "$A/$t.elf" -nojit > a64nojit-$t.out 2>&1 || jitok=0
  [ "$(sed 's/ in [0-9.]* s.*//' a64jit-$t.out)" = "$(sed 's/ in [0-9.]* s.*//' a64nojit-$t.out)" ] || { jitok=0; echo "      $t differs"; }
done
MVPLAT_JIT_HOT=1 "$MVPLAT" "$A/smpmt.elf" -harts 4 -mt -q > a64jit-smpmt.out 2>&1 && grep -q "smpmt: all passed" a64jit-smpmt.out || jitok=0
[ $jitok = 1 ] && ok "aarch64: translated to host code, the same results and instruction counts as -nojit" ||
  bad "aarch64: translated blocks differ from -nojit"
MVPLAT_NO_DECODE=1 "$MVPLAT" "$A/smc.elf" -q > a64smc2.out 2>&1; rc=$?
expect "aarch64: the same with nothing decoded ahead (MVPLAT_NO_DECODE)" 0 a64smc2.out "smc: all passed"
"$MVPLAT" "$A/hello.elf" -bios "$A/hello.elf" -q > a64bios.out 2>&1; rc=$?
expect "aarch64: -bios is refused" 2 a64bios.out "bios"
# Random instruction blocks against QEMU's user-mode emulator, run as a
# separate program (when qemu-aarch64, clang and ld.lld are installed).
QEMU_A64=$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)
if [ -n "$QEMU_A64" ] && command -v clang > /dev/null 2>&1 && command -v ld.lld > /dev/null 2>&1; then
  if bash "$T/a64/difftest/difftest.sh" "$MVPLAT" "$QEMU_A64" difftest 1 1500 4 > difftest.out 2>&1; then
    ok "aarch64: 1500 random integer, NEON and floating-point blocks agree with qemu-aarch64"
  else bad "aarch64: random blocks differ from qemu-aarch64"; tail -30 difftest.out | sed 's/^/      /'; fi
  if MVPLAT_JIT_HOT=1 bash "$T/a64/difftest/difftest.sh" "$MVPLAT" "$QEMU_A64" difftest-jit 2 500 4 > difftest-jit.out 2>&1; then
    ok "aarch64: 500 random blocks translated to host code agree with qemu-aarch64"
  else bad "aarch64: translated random blocks differ from qemu-aarch64"; tail -30 difftest-jit.out | sed 's/^/      /'; fi
else
  echo "skip  aarch64 random blocks: needs qemu-aarch64, clang and ld.lld"
fi
if command -v dtc > /dev/null 2>&1; then
  dtc -I dtb -O dts board.dtb > board.dts 2> dtc.err; rc=$?
  if [ $rc = 0 ] && ! [ -s dtc.err ] && grep -q 'cpu@1' board.dts && grep -q 'riscv,sv32' board.dts &&
     grep -q 'ns16550a' board.dts && grep -q 'riscv,plic0' board.dts && grep -q 'syscon-poweroff' board.dts && grep -q 'virtio,mmio' board.dts; then
    ok "the device tree decompiles cleanly with dtc and has the harts and devices"
  else bad "dtc on the device tree"; cat dtc.err; fi
fi
"$MVPLAT" "$E/hello.elf" -initrd x > p.out 2>&1; rc=$?
expect "-initrd without -kernel" 2 p.out "need -kernel or -bios"

# OpenSBI (the distribution's fw_dynamic) starts a supervisor-mode payload;
# the payload starts the other harts through SBI HSM and shuts down via SRST.
BIOS=${MVPLAT_BIOS:-}
for b in /usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.elf /usr/share/opensbi/lp64/generic/firmware/fw_dynamic.elf; do
  [ -z "$BIOS" ] && [ -f "$b" ] && BIOS=$b
done
if [ -n "$BIOS" ]; then
  for n in 1 4; do
    "$MVPLAT" -bios "$BIOS" -kernel "$E/sbi64.elf" -harts $n -q > sbi$n.out 2>&1; rc=$?
    expect "OpenSBI boots an S-mode payload on $n hart(s)" 0 sbi$n.out "sbi: harts running: $n"
    grep -q "sbi: PASS" sbi$n.out || bad "S-mode payload on $n hart(s) did not pass"
  done
  grep -q "Platform Name *: minisim mvplat" sbi1.out && ok "OpenSBI read the platform's device tree" || bad "OpenSBI platform name"
else
  echo "skip  OpenSBI tests: no fw_dynamic.elf (apt install opensbi, or set MVPLAT_BIOS)"
fi

# A stock Linux kernel to user space and power-off (tests/vplat/get-linux.sh
# fetches one): MVPLAT_LINUX_KERNEL=.../Image.
if [ -n "${MVPLAT_LINUX_KERNEL:-}" ] && [ -n "$BIOS" ]; then
  printf '%s\n' '{"devices": [{"type":"finisher","base":"0x100000"},{"type":"clint","base":"0x2000000"},{"type":"plic","base":"0xc000000"},{"type":"uart16550","base":"0x10000000","irq":10},{"type":"ram","base":"0x80000000","size":"512M"}]}' > linux.json
  "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd "$E/initramfs64.cpio" \
    -append "console=ttyS0 rdinit=/init" -harts 2 -parallel -q > linux.out 2>&1; rc=$?
  expect "Linux boots to /init on 2 harts in parallel rounds and powers off" 0 linux.out "mvplat-init: powering off"
  "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd "$E/initramfs64.cpio" \
    -append "console=ttyS0 rdinit=/init" -harts 4 -q > linux_serial.out 2>&1; rc=$?
  expect "Linux on 4 harts (taking turns, the default)" 0 linux_serial.out "mvplat-init: powering off"
  grep -q "smp: Brought up 1 node, 4 CPUs" linux_serial.out && ok "Linux -serial brought up 4 harts" || bad "Linux -serial SMP bring-up"
  "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd "$E/initramfs64.cpio" \
    -append "console=ttyS0 rdinit=/init" -harts 4 -mt -q > linux_mt.out 2>&1; rc=$?
  expect "Linux on 4 harts with -mt (a thread each, one shared translator)" 0 linux_mt.out "mvplat-init: powering off"
  grep -q "smp: Brought up 1 node, 4 CPUs" linux_mt.out && ok "Linux -mt brought up 4 harts" || bad "Linux -mt SMP bring-up"
  grep -q "^Linux version" linux.out && grep -q "smp: Brought up 1 node, 2 CPUs" linux.out &&
    ok "Linux brought up both harts and printed /proc/version" || bad "Linux SMP bring-up"
  # An rv64gc user space (floating point included): BusyBox's shell runs
  # what is typed on the console once the kernel's driver is up.
  if [ -n "${MVPLAT_BUSYBOX:-}" ] && command -v python3 > /dev/null 2>&1; then
    python3 "$T/mkcpio.py" bb.cpio d:dev c:dev/console:5:1 c:dev/null:1:3 d:bin d:mnt "f:bin/busybox:$MVPLAT_BUSYBOX" \
      "f:init:$T/busybox_init.sh" "f:bin/vec:$E/vec_linux64"
    "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd bb.cpio \
      -append "console=ttyS0 rdinit=/init quiet" -harts 2 -q \
      -input "echo typed \$((6*7)); busybox printf '%.3f\n' 3.14159; poweroff -f
" > bb.out 2>&1; rc=$?
    expect "BusyBox shell on Linux: rv64gc user space and console input" 0 bb.out "^typed 42"
    grep -q "^3.142" bb.out && ok "floating point in user space (printf %.3f)" || bad "user-space floating point"
    # A disk: Linux's virtio-blk driver on -disk, an ext4 file system made
    # here (mkfs.ext4 -d), mounted, read and written back to the image; then
    # the same kind of disk as the root file system, with no initramfs.
    if command -v mkfs.ext4 > /dev/null 2>&1 && command -v debugfs > /dev/null 2>&1; then
      rm -rf fsroot; mkdir -p fsroot; echo "hello from the disk" > fsroot/hello.txt
      mkfs.ext4 -q -F -d fsroot data.img 8M > /dev/null 2>&1
      "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd bb.cpio \
        -append "console=ttyS0 rdinit=/init quiet" -disk data.img -disk-write -q \
        -input "mount -t devtmpfs d /dev; mount /dev/vda /mnt; cat /mnt/hello.txt; echo written > /mnt/new.txt; umount /mnt; poweroff -f
" > ext4.out 2>&1; rc=$?
      expect "Linux mounts an ext4 disk on virtio-blk and reads a file" 0 ext4.out "^hello from the disk"
      [ "$(debugfs -R 'cat /new.txt' data.img 2> /dev/null)" = written ] &&
        ok "a file Linux wrote is in the image (-disk-write)" || bad "the file Linux wrote is not in the image"
      rm -rf rootfs; mkdir -p rootfs/bin rootfs/dev rootfs/proc
      cp "$MVPLAT_BUSYBOX" rootfs/bin/busybox; ln -s busybox rootfs/bin/sh
      mkfs.ext4 -q -F -d rootfs root.img 16M > /dev/null 2>&1
      "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -disk root.img -q \
        -append "console=ttyS0 root=/dev/vda rw init=/bin/sh quiet" \
        -input "/bin/busybox echo root on disk: \$(/bin/busybox ls /bin); /bin/busybox poweroff -f
" > rootfs.out 2>&1; rc=$?
      expect "Linux boots with its root file system on virtio-blk" 0 rootfs.out "^root on disk: busybox sh"
      # The same disk behind a PCIe host: Linux's pci-host-generic finds the
      # device, assigns its BAR, and virtio-pci drives it with INTx.
      printf '%s\n' '{"devices": [{"type":"finisher","base":"0x100000"},{"type":"clint","base":"0x2000000"},{"type":"plic","base":"0xc000000","sources":95},{"type":"uart16550","base":"0x10000000","irq":10},{"type":"ram","base":"0x80000000","size":"512M"},{"type":"pcie","base":"0x30000000","irq":32,"devices":[{"type":"virtio-blk","file":"data.img"}]}]}' > linux_pci.json
      "$MVPLAT" -platform linux_pci.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd bb.cpio \
        -append "console=ttyS0 rdinit=/init quiet" -q \
        -input "mount -t devtmpfs d /dev; cat /sys/bus/pci/devices/0000:00:01.0/vendor; mount /dev/vda /mnt; cat /mnt/hello.txt; poweroff -f
" > pcidisk.out 2>&1; rc=$?
      expect "Linux mounts a virtio-blk disk behind the PCIe host" 0 pcidisk.out "^hello from the disk"
      grep -q "^0x1af4" pcidisk.out && ok "Linux enumerated the virtio device at 00:01.0" || bad "PCI enumeration under Linux"
    else
      echo "skip  ext4 disk: no mkfs.ext4 and debugfs (e2fsprogs)"
    fi
    # Sv48: with the device tree offering it, Linux pages with 4 levels.
    "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd bb.cpio -q \
      -append "console=ttyS0 rdinit=/init quiet" -mmu sv48 -input "grep -m1 mmu /proc/cpuinfo; poweroff -f
" > sv48.out 2>&1; rc=$?
    expect "Linux runs with Sv48 paging (-mmu sv48)" 0 sv48.out "^mmu.*: sv48"
    # Vector code in three processes at once on one hart: Linux turns the
    # vector unit on for each and switches their vector registers.
    "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd bb.cpio -q \
      -append "console=ttyS0 rdinit=/init quiet" -input "grep -m1 isa /proc/cpuinfo; /bin/vec & /bin/vec & /bin/vec; wait; poweroff -f
" > vlinux.out 2>&1; rc=$?
    expect "Linux runs vector code in three processes" 0 vlinux.out "^isa.*: rv64imafdcv_"
    [ "$(grep -c '51 of 51 kernel runs agree under Linux' vlinux.out)" = 3 ] &&
      ok "each process's vector results agree with its scalar ones" || { bad "vector code under Linux"; sed 's/^/      /' vlinux.out; }
    # A network: Linux's virtio-net driver brings eth0 up and pings the
    # built-in gateway.
    "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd bb.cpio -q \
      -append "console=ttyS0 rdinit=/init quiet" -net user \
      -input "ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up; ping -c 2 10.0.2.2; poweroff -f
" > net.out 2>&1; rc=$?
    expect "Linux pings the gateway over virtio-net" 0 net.out "2 packets transmitted, 2 packets received"
    # Linux's console on virtio-console (hvc0) instead of the 16550.
    "$MVPLAT" -platform linux.json -bios "$BIOS" -kernel "$MVPLAT_LINUX_KERNEL" -initrd bb.cpio -q \
      -append "console=hvc0 rdinit=/init quiet" -vcon -input "echo on hvc \$((6*7)); poweroff -f
" > hvc.out 2>&1; rc=$?
    expect "Linux runs a shell on hvc0 (virtio-console)" 0 hvc.out "^on hvc 42"
  else
    echo "skip  BusyBox: set MVPLAT_BUSYBOX (tests/vplat/get-linux.sh fetches it)"
  fi
else
  echo "skip  Linux boot: set MVPLAT_LINUX_KERNEL (tests/vplat/get-linux.sh fetches a kernel)"
fi

# Linux on AArch64: Ubuntu's arm64 kernel and BusyBox on 2 processors, on
# the built-in arm board (tests/vplat/get-linux-arm64.sh fetches them).
if [ -n "${MVPLAT_ARM64_KERNEL:-}" ] && [ -n "${MVPLAT_ARM64_BUSYBOX:-}" ] && command -v python3 > /dev/null 2>&1; then
  python3 "$T/mkcpio.py" bb64.cpio d:dev c:dev/console:5:1 c:dev/null:1:3 d:bin d:mnt "f:bin/busybox:$MVPLAT_ARM64_BUSYBOX" \
    "f:init:$T/busybox_init.sh"
  "$MVPLAT" -kernel "$MVPLAT_ARM64_KERNEL" -initrd bb64.cpio -append "console=ttyAMA0 rdinit=/init quiet" -harts 2 -q \
    -input "echo arm \$((6*7)); uname -m; poweroff -f
" > linux-arm64.out 2>&1; rc=$?
  expect "Linux arm64 and a BusyBox shell on 2 AArch64 processors" 0 linux-arm64.out "shell on aarch64, 2 harts"
  grep -q "^arm 42" linux-arm64.out && ok "the arm64 shell runs what is typed" || { bad "the arm64 shell"; sed 's/^/      /' linux-arm64.out | tail -20; }
  "$MVPLAT" -kernel "$MVPLAT_ARM64_KERNEL" -initrd bb64.cpio -append "console=ttyAMA0 rdinit=/init quiet" -harts 4 -mt -q \
    -input "echo mt \$((6*7)); poweroff -f
" > linux-arm64-mt.out 2>&1; rc=$?
  expect "Linux arm64 on 4 processors with -mt" 0 linux-arm64-mt.out "^mt 42"
  # virtio-mmio from -disk, -net and -vcon; virtio over PCIe from a platform file
  head -c 65536 /dev/zero > a64disk.img; printf MVPLATDISK | dd of=a64disk.img conv=notrunc 2> /dev/null
  "$MVPLAT" -kernel "$MVPLAT_ARM64_KERNEL" -initrd bb64.cpio -append "console=ttyAMA0 rdinit=/init quiet" -q \
    -disk a64disk.img -net user -input "mount -t devtmpfs d /dev; head -c 10 /dev/vda; echo; ip link set eth0 up; ip addr add 10.0.2.15/24 dev eth0; ping -c 2 10.0.2.2; poweroff -f
" > linux-arm64-virtio.out 2>&1; rc=$?
  expect "arm64: Linux reads virtio-blk (-disk)" 0 linux-arm64-virtio.out "^MVPLATDISK"
  grep -q "2 packets transmitted, 2 packets received" linux-arm64-virtio.out && ok "arm64: Linux pings the gateway over virtio-net (-net user)" ||
    { bad "arm64: virtio-net"; tail -20 linux-arm64-virtio.out | sed 's/^/      /'; }
  "$MVPLAT" -kernel "$MVPLAT_ARM64_KERNEL" -initrd bb64.cpio -append "console=hvc0 rdinit=/init quiet" -q -vcon \
    -input "echo on hvc \$((6*7)); poweroff -f
" > linux-arm64-hvc.out 2>&1; rc=$?
  expect "arm64: a shell on hvc0 (-vcon)" 0 linux-arm64-hvc.out "^on hvc 42"
  cp "$T/a64/pcie.json" a64pcie.json; cp a64disk.img disk.img
  "$MVPLAT" -platform a64pcie.json -kernel "$MVPLAT_ARM64_KERNEL" -initrd bb64.cpio -append "console=ttyAMA0 rdinit=/init quiet" -q \
    -input "mount -t devtmpfs d /dev; head -c 10 /dev/vda; echo; ls /sys/bus/pci/devices; poweroff -f
" > linux-arm64-pcie.out 2>&1; rc=$?
  expect "arm64: virtio-blk behind the PCIe host (2 processors)" 0 linux-arm64-pcie.out "^MVPLATDISK"
  grep -q "0000:00:01.0  0000:00:02.0" linux-arm64-pcie.out && ok "arm64: both PCIe devices enumerated" || bad "arm64: PCIe enumeration"
else
  echo "skip  Linux arm64: set MVPLAT_ARM64_KERNEL and MVPLAT_ARM64_BUSYBOX (tests/vplat/get-linux-arm64.sh fetches them)"
fi

exit $fail
