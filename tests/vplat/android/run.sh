#!/bin/sh
# run.sh IMAGES WORK [mvplat options...] - boot an aosp_cf_riscv64 or
# aosp_cf_arm64 build on mvplat. IMAGES holds the build's boot.img,
# init_boot.img, vendor_boot.img, vbmeta*.img, super.img and userdata.img,
# and for riscv64 the host package's
# bootloader/opensbi-riscv64-generic-fw_dynamic.bin (or set MVPLAT_SBI).
# WORK gets the disk (sparse, about 9 GiB apparent), the initrd, the kernel,
# the console log (console.log) and, when the run ends, the screen
# (screen.ppm), and cf/ the host end of the console ports and what the
# guest wrote to them (cf/hvc2.log is logcat). See docs/ANDROID.md.
set -e
here=$(cd "$(dirname "$0")" && pwd)
img=$(cd "$1" && pwd); mkdir -p "$2"; work=$(cd "$2" && pwd); shift 2
mvplat=$(realpath "${MVPLAT:-$here/../../../build/mvplat}")
# an arm64 kernel Image has "ARM\x64" at byte 56 (its header follows boot.img's 4 KiB header)
if [ "$(dd if="$img/boot.img" bs=1 skip=4152 count=4 2> /dev/null)" = "$(printf 'ARMd')" ]; then arch=arm64; else arch=riscv64; fi
if [ $arch = riscv64 ]; then
  sbi=${MVPLAT_SBI:-$(ls "$img"/opensbi-riscv64-generic-fw_dynamic.bin "$img"/host/opensbi-riscv64-generic-fw_dynamic.bin 2>/dev/null | head -1)}
  [ -f "$sbi" ] || { echo "run.sh: no OpenSBI fw_dynamic.bin; set MVPLAT_SBI" >&2; exit 1; }
  plat=android.json; boot="-mt -bios $sbi"; con="8250.nr_uarts=1 console=ttyS0 earlycon=sbi"; bc=
else
  # the arm board: PL011 console, the PCIe host's ECAM at 0x3f000000
  plat=android-arm64.json; boot=-mt; con="console=ttyAMA0 earlycon"
  bc="androidboot.console=ttyAMA0 androidboot.boot_devices=pci0000:00/0000:00:01.0,3f000000.pcie"
fi

cd "$work"
[ -f android.disk ] || python3 -I "$here/mkdisk.py" android.disk "$img"
[ -f android.initrd ] || python3 -I "$here/mkinitrd.py" android.initrd "$img/init_boot.img" "$img/vendor_boot.img" $bc
[ -f kernel ] || python3 -I - "$img/boot.img" <<'P'
import struct, sys
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'ANDROID!'
n, = struct.unpack_from('<I', b, 8)
open('kernel', 'wb').write(b[4096:4096 + n])
P
cp "$here/$plat" .
# FRAMES=MS: the screen to frames/screen-<simulated ms>.ppm every MS while it changes
if [ -n "${FRAMES:-}" ]; then
  mkdir -p frames
  sed -i "s|\"screen\": \"screen.ppm\"|&, \"frames\": \"frames/screen\", \"frame_ms\": $FRAMES|" "$plat"
fi
# The host end of the console ports the HALs talk to (cfhost.py).
mkdir -p cf
rm -f cf/hvc*.sock  # a previous run's, which nothing listens on
python3 -I "$here/cfhost.py" "$work/cf" > cf/cfhost.log 2>&1 &
host=$!
trap 'kill $host 2> /dev/null' EXIT
for i in $(seq 50); do [ -S cf/hvc19.sock ] && break; sleep 0.1; done
# HALs whose host programs are not there (the radio, Bluetooth, UWB) crash
# and are restarted; one in an APEX that crashes 4 times before the boot
# completes makes apexd roll back and reboot. Stop what is being restarted,
# from the shell, every 25 simulated seconds until system_server is up
# (STOP_LOOP=0 to leave them).
stop='for p in $(getprop | grep -E "^.init.svc.[^]]*.: .restarting" | sed -E "s/^.init.svc.([^]]*).*/\1/"); do su 0 setprop ctl.stop $p; echo stopped $p; done
'
if [ "${STOP_LOOP:-1}" != 0 ]; then
  for s in $(seq 400 25 2000); do set -- -input-at "$s:$stop" "$@"; done
fi
"$mvplat" -platform $plat -harts ${HARTS:-4} $boot -kernel kernel -initrd android.initrd \
  -append "printk.devkmsg=on audit=0 panic=-1 binder.impl=rust cma=0 firmware_class.path=/vendor/etc/ loop.max_part=7 init=/init bootconfig $con" \
  "$@" > console.log 2>&1
