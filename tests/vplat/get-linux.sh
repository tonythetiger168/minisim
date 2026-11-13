#!/bin/sh
# get-linux.sh - fetch a stock RISC-V Linux kernel for the Linux boot test:
# Ubuntu 24.04's riscv64 generic kernel (an uncompressed Image), from the
# Ubuntu ports archive. OpenSBI comes from the distribution (apt install opensbi).
#   tests/vplat/get-linux.sh DIR      -> DIR/Image
# Then: MVPLAT_LINUX_KERNEL=DIR/Image ctest -R mvplat
set -e
dir=${1:?usage: get-linux.sh DIR}
mkdir -p "$dir"
cd "$dir"
deb=linux-image-6.8.0-60-generic_6.8.0-60.63.1_riscv64.deb
[ -f "$deb" ] || curl -fsSL -o "$deb" "http://ports.ubuntu.com/ubuntu-ports/pool/main/l/linux-riscv/$deb"
rm -rf x
dpkg-deb -x "$deb" x
cp x/boot/vmlinuz-* Image
rm -rf x
echo "$dir/Image"
