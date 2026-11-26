#!/bin/sh
# run.sh IMAGES WORK [mvplat options...] - boot an aosp_cf_riscv64 build on
# mvplat. IMAGES holds the build's boot.img, init_boot.img, vendor_boot.img,
# vbmeta*.img, super.img and userdata.img, and the host package's
# bootloader/opensbi-riscv64-generic-fw_dynamic.bin (or set MVPLAT_SBI).
# WORK gets the disk (sparse, about 9 GiB apparent), the initrd, the kernel,
# the console log (console.log) and, when the run ends, the screen
# (screen.ppm). See docs/ANDROID.md.
set -e
here=$(cd "$(dirname "$0")" && pwd)
img=$(cd "$1" && pwd); mkdir -p "$2"; work=$(cd "$2" && pwd); shift 2
mvplat=$(realpath "${MVPLAT:-$here/../../../build/mvplat}")
sbi=${MVPLAT_SBI:-$(ls "$img"/opensbi-riscv64-generic-fw_dynamic.bin "$img"/host/opensbi-riscv64-generic-fw_dynamic.bin 2>/dev/null | head -1)}
[ -f "$sbi" ] || { echo "run.sh: no OpenSBI fw_dynamic.bin; set MVPLAT_SBI" >&2; exit 1; }

cd "$work"
[ -f android.disk ] || python3 -I "$here/mkdisk.py" android.disk "$img"
[ -f android.initrd ] || python3 -I "$here/mkinitrd.py" android.initrd "$img/init_boot.img" "$img/vendor_boot.img"
[ -f kernel ] || python3 -I - "$img/boot.img" <<'P'
import struct, sys
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'ANDROID!'
n, = struct.unpack_from('<I', b, 8)
open('kernel', 'wb').write(b[4096:4096 + n])
P
cp "$here/android.json" .
exec "$mvplat" -platform android.json -harts ${HARTS:-2} -mt -bios "$sbi" -kernel kernel -initrd android.initrd \
  -append "printk.devkmsg=on audit=1 panic=-1 8250.nr_uarts=1 binder.impl=rust cma=0 firmware_class.path=/vendor/etc/ loop.max_part=7 init=/init bootconfig console=ttyS0 earlycon=sbi" \
  "$@" > console.log 2>&1
