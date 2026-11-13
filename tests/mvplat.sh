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
    -append "console=ttyS0 rdinit=/init" -harts 2 -q > linux.out 2>&1; rc=$?
  expect "Linux boots to /init on 2 harts and powers off" 0 linux.out "mvplat-init: powering off"
  grep -q "^Linux version" linux.out && grep -q "smp: Brought up 1 node, 2 CPUs" linux.out &&
    ok "Linux brought up both harts and printed /proc/version" || bad "Linux SMP bring-up"
  # An rv64gc user space (floating point included): BusyBox's shell runs
  # what is typed on the console once the kernel's driver is up.
  if [ -n "${MVPLAT_BUSYBOX:-}" ] && command -v python3 > /dev/null 2>&1; then
    python3 "$T/mkcpio.py" bb.cpio d:dev c:dev/console:5:1 d:bin d:mnt "f:bin/busybox:$MVPLAT_BUSYBOX" "f:init:$T/busybox_init.sh"
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
    else
      echo "skip  ext4 disk: no mkfs.ext4 and debugfs (e2fsprogs)"
    fi
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

exit $fail
