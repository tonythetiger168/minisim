#!/bin/busybox sh
# busybox_init.sh - /init of the initramfs for the BusyBox test: the applets,
# /proc and /sys, a line about the system, then a shell on the console.
/bin/busybox mkdir -p /proc /sys /sbin /usr/bin /usr/sbin
/bin/busybox --install -s
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "mvplat: busybox $(busybox | head -1 | cut -d' ' -f2) shell on $(uname -m), $(grep -c ^processor /proc/cpuinfo) harts"
exec sh
