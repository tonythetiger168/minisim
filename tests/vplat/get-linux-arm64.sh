#!/bin/sh
# get-linux-arm64.sh - fetch a stock arm64 Linux kernel for the AArch64 Linux
# boot test: Ubuntu 24.04's arm64 generic kernel (its gzip-compressed Image,
# uncompressed here) and its static BusyBox, from the Ubuntu ports archive.
# They are test inputs, run inside mvplat; nothing of them is shipped.
#   tests/vplat/get-linux-arm64.sh DIR      -> DIR/Image-arm64, DIR/busybox-arm64
# Then: MVPLAT_ARM64_KERNEL=DIR/Image-arm64 MVPLAT_ARM64_BUSYBOX=DIR/busybox-arm64 ctest -R mvplat
set -e
dir=${1:?usage: get-linux-arm64.sh DIR}
mkdir -p "$dir"
cd "$dir"
deb=linux-image-unsigned-6.8.0-71-generic_6.8.0-71.71_arm64.deb
[ -f "$deb" ] || curl -fsSL -o "$deb" "http://ports.ubuntu.com/ubuntu-ports/pool/main/l/linux/$deb"
rm -rf x
dpkg-deb -x "$deb" x
gzip -dc x/boot/vmlinuz-* > Image-arm64
rm -rf x
bb=busybox-static_1.36.1-6ubuntu3.1_arm64.deb
[ -f "$bb" ] || curl -fsSL -o "$bb" "http://ports.ubuntu.com/ubuntu-ports/pool/main/b/busybox/$bb"
dpkg-deb -x "$bb" x
cp x/usr/bin/busybox busybox-arm64
rm -rf x
echo "$dir/Image-arm64 $dir/busybox-arm64"
