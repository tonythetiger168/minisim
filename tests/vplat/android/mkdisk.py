#!/usr/bin/env python3
# mkdisk.py OUT IMGDIR - a GPT disk for the Android guest from the images of an
# aosp_cf_riscv64 build: the partitions Cuttlefish's os_composite has, named
# as first-stage init looks for them (/dev/block/by-name/<name>). Android
# sparse images are expanded; the output is a sparse host file (holes for zeros).
import os, struct, sys, uuid, zlib

out, d = sys.argv[1], sys.argv[2]
SEC = 512
ALIGN = 1 << 20


def sparse_size(path):
    with open(path, 'rb') as f:
        h = f.read(28)
    if struct.unpack('<I', h[:4])[0] != 0xed26ff3a:
        return os.path.getsize(path), False
    _, _, _, fhs, chs, blk, nblk, nch, _ = struct.unpack('<IHHHHIIII', h)
    return blk * nblk, True


def write_image(fo, off, path):
    """Copy an image (raw or Android sparse) to fo at off."""
    size, sp = sparse_size(path)
    with open(path, 'rb') as f:
        if not sp:
            fo.seek(off)
            while True:
                b = f.read(1 << 22)
                if not b:
                    break
                fo.write(b)
            return
        h = f.read(28)
        _, _, _, fhs, chs, blk, nblk, nch, _ = struct.unpack('<IHHHHIIII', h)
        f.seek(fhs)
        pos = off
        for _ in range(nch):
            ch = f.read(chs)
            typ, _, sz, total = struct.unpack('<HHII', ch[:12])
            n = sz * blk
            if typ == 0xcac1:  # raw
                fo.seek(pos)
                left = n
                while left:
                    b = f.read(min(left, 1 << 22))
                    fo.write(b)
                    left -= len(b)
            elif typ == 0xcac2:  # fill
                v = f.read(4)
                if v != b'\0\0\0\0':
                    fo.seek(pos)
                    fo.write(v * (n // 4))
            elif typ == 0xcac3:  # don't care
                pass
            elif typ == 0xcac4:  # crc
                f.read(4)
            pos += n


# name, image file (or None), size when there is no image
parts = [
    ('misc', None, 1 << 20),
    ('frp', None, 1 << 20),
    ('boot_a', 'boot.img', 0),
    ('init_boot_a', 'init_boot.img', 0),
    ('vendor_boot_a', 'vendor_boot.img', 0),
    ('vbmeta_a', 'vbmeta.img', 0),
    ('vbmeta_system_a', 'vbmeta_system.img', 0),
    ('vbmeta_vendor_dlkm_a', 'vbmeta_vendor_dlkm.img', 0),
    ('vbmeta_system_dlkm_a', 'vbmeta_system_dlkm.img', 0),
    ('super', 'super.img', 0),
    ('metadata', None, 64 << 20),
    ('userdata', 'userdata.img', 0),
]
lay = []
lba = ALIGN // SEC
for name, img, size in parts:
    if img:
        size, _ = sparse_size(os.path.join(d, img))
    size = (size + ALIGN - 1) // ALIGN * ALIGN
    lay.append((name, img, lba, size // SEC))
    lba += size // SEC
total = lba + 34 + (ALIGN // SEC)
LINUX = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4')

ents = b''
for i, (name, img, first, n) in enumerate(lay):
    pu = uuid.uuid5(uuid.NAMESPACE_DNS, 'mvplat-android-' + name)
    nm = name.encode('utf-16-le').ljust(72, b'\0')
    ents += LINUX.bytes_le + pu.bytes_le + struct.pack('<QQQ', first, first + n - 1, 0) + nm
ents = ents.ljust(128 * 128, b'\0')
ecrc = zlib.crc32(ents) & 0xffffffff
disk = uuid.uuid5(uuid.NAMESPACE_DNS, 'mvplat-android-disk')


def header(cur, alt, entlba):
    h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0, cur, alt, 34, total - 34,
                    disk.bytes_le, entlba, 128, 128, ecrc)
    c = zlib.crc32(h) & 0xffffffff
    h = h[:16] + struct.pack('<I', c) + h[20:]
    return h.ljust(SEC, b'\0')


with open(out, 'wb') as fo:
    fo.truncate(total * SEC)
    mbr = bytearray(SEC)
    mbr[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 2, 0, 0xee, 0xff, 0xff, 0xff, 1, min(total - 1, 0xffffffff))
    mbr[510:512] = b'\x55\xaa'
    fo.seek(0); fo.write(mbr)
    fo.seek(SEC); fo.write(header(1, total - 1, 2))
    fo.write(ents)
    fo.seek((total - 33) * SEC); fo.write(ents)
    fo.seek((total - 1) * SEC); fo.write(header(total - 1, 1, total - 33))
    for name, img, first, n in lay:
        if img:
            write_image(fo, first * SEC, os.path.join(d, img))
        print('%-22s %8d MiB at LBA %d' % (name, n * SEC >> 20, first))
print('%s: %d MiB' % (out, total * SEC >> 20))
