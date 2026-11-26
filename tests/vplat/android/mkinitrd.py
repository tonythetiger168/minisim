#!/usr/bin/env python3
# mkinitrd.py OUT INIT_BOOT VENDOR_BOOT [key=value...]
#
# The initrd an Android bootloader would hand the kernel (boot image header
# v4): the vendor ramdisk, then the generic ramdisk of init_boot, then a
# bootconfig trailer (Linux Documentation/admin-guide/bootconfig.rst). The
# trailer is vendor_boot's own bootconfig, then the entries mvplat needs,
# then the key=value arguments (a key given again replaces the earlier one).
# With OUT already an initrd of this form, INIT_BOOT VENDOR_BOOT may be "-":
# only the trailer is rewritten.
import struct, sys


def pad(n, p):
    return (n + p - 1) // p * p


def ramdisk(path):
    """The ramdisk of a boot image v3/v4 (init_boot.img)."""
    b = open(path, 'rb').read()
    if b[:8] != b'ANDROID!':
        sys.exit('%s: not an Android boot image' % path)
    ksize, rsize = struct.unpack_from('<2I', b, 8)
    page = 4096
    o = page + pad(ksize, page)
    return b[o:o + rsize]


def vendor(path):
    """The vendor ramdisks and bootconfig of a vendor_boot image v4."""
    b = open(path, 'rb').read()
    if b[:8] != b'VNDRBOOT':
        sys.exit('%s: not a vendor boot image' % path)
    ver, page, _, _, vrsize = struct.unpack_from('<5I', b, 8)
    o = 28 + 2048 + 4 + 16
    hsize, dtbsize = struct.unpack_from('<II', b, o)
    o += 16
    tsize, _, _, bcsize = struct.unpack_from('<4I', b, o)
    p = pad(hsize, page)
    vr = b[p:p + vrsize]
    p += pad(vrsize, page) + pad(dtbsize, page) + pad(tsize, page)
    bc = b[p:p + bcsize].rstrip(b'\0').decode()
    return vr, [l for l in bc.split('\n') if l]


# What the Cuttlefish host tools would add for this machine.
MVPLAT = [
    'androidboot.slot_suffix=_a',
    'androidboot.force_normal_boot=1',
    'androidboot.verifiedbootstate=orange',
    'androidboot.serialconsole=1',
    'androidboot.console=ttyS0',
    'androidboot.selinux=permissive',
    # The disk: virtio-blk at 01.0 behind the pcie host at 0x30000000.
    'androidboot.boot_devices=pci0000:00/0000:00:01.0,30000000.pci,soc/30000000.pci',
    'androidboot.fstab_suffix=cf.f2fs.hctr2',
    # No secure world: the software keymint and gatekeeper.
    'androidboot.vendor.apex.com.android.hardware.gatekeeper=com.android.hardware.gatekeeper.nonsecure.apex',
    'androidboot.vendor.apex.com.android.hardware.keymint=com.android.hardware.keymint.rust_nonsecure.apex',
    # Graphics: virtio-gpu 2D (DRM dumb buffers), software rendering.
    'androidboot.hardware.gralloc=minigbm',
    'androidboot.hardware.hwcomposer=ranchu',
    'androidboot.hardware.hwcomposer.mode=guest',
    'androidboot.hardware.hwcomposer.display_finder_mode=drm',
    'androidboot.vendor.apex.com.android.hardware.graphics.composer=com.android.hardware.graphics.composer.ranchu.apex',
    'androidboot.hardware.egl=angle',
    'androidboot.hardware.vulkan=pastel',
    'androidboot.cpuvulkan.version=4202496',
    'androidboot.opengles.version=196608',
    'androidboot.lcd_density=320',
    'androidboot.vendor.apex.com.google.emulated.camera.provider.hal=com.google.emulated.camera.provider.hal.apex',
    # A simulated CPU is slower than a phone's: stretch the framework's
    # watchdogs and timeouts (Build.HW_TIMEOUT_MULTIPLIER).
    'androidboot.hw_timeout_multiplier=20',
    # The lights HAL listens on this vsock port; unset, it asks for port 0,
    # which only root may bind, aborts, and system_server waits for it.
    'androidboot.vsock_lights_port=6900',
]


def main():
    if len(sys.argv) < 4:
        sys.exit('usage: mkinitrd.py OUT INIT_BOOT VENDOR_BOOT [key=value...]')
    out, ib, vb = sys.argv[1:4]
    if ib == '-':
        d = open(out, 'rb').read()
        if not d.endswith(b'#BOOTCONFIG\n'):
            sys.exit('%s: no bootconfig trailer' % out)
        size, _ = struct.unpack('<II', d[-20:-12])
        old = [l for l in d[-20 - size:-20].rstrip(b'\0').decode().split('\n') if l]
        d = d[:-20 - size]
    else:
        vr, old = vendor(vb)
        old += MVPLAT
        d = vr + b'\0' * (-len(vr) % 4)
        r = ramdisk(ib)
        d += r + b'\0' * (-len(r) % 4)
    kv = {}
    for l in old + sys.argv[4:]:
        k, v = l.split('=', 1)
        kv[k.strip()] = v.strip()
    body = ''.join('%s=%s\n' % (k, v) for k, v in kv.items()).encode() + b'\0'
    body += b'\0' * (-len(body) % 4)
    with open(out, 'wb') as f:
        f.write(d + body + struct.pack('<II', len(body), sum(body) & 0xffffffff) + b'#BOOTCONFIG\n')
    print('%s: %d bytes, %d bootconfig entries' % (out, len(d) + len(body) + 20, len(kv)))


main()
