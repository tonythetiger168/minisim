# Android on mvplat

mvplat boots an unmodified Android build: the riscv64 Cuttlefish phone image
that AOSP's continuous integration publishes (`aosp_cf_riscv64_phone-userdebug`,
checked with build 16968706, kernel 6.12). Nothing in the image is changed:
the disk holds the build's own partitions, the kernel and ramdisks are the
build's own, and what a bootloader would add (the bootconfig) is made by a
script.

## What you need

From the build's artifacts:

- `boot.img`, `init_boot.img`, `vendor_boot.img`, `vbmeta.img`,
  `vbmeta_system.img`, `vbmeta_system_dlkm.img`, `vbmeta_vendor_dlkm.img`,
  `super.img`, `userdata.img` (in the `*-img-*.zip` and the target files);
- OpenSBI's `opensbi-riscv64-generic-fw_dynamic.bin` from the host package
  (`cvd-host_package.tar.gz`, under `bootloader/` or `host/`). Any OpenSBI
  1.x `fw_dynamic` works; set `MVPLAT_SBI` to use another.

Put them in one directory, then:

```sh
tests/vplat/android/run.sh IMAGES WORK -max-cycles 300000000000
```

`WORK` gets:

| File | What it is |
|---|---|
| `android.disk` | a GPT disk (sparse, 16 GiB apparent, about 1.4 GiB used) with the partitions Cuttlefish's composite disk has: `misc`, `frp`, `boot_a`, `init_boot_a`, `vendor_boot_a`, the `vbmeta*_a`, `super`, `metadata`, `userdata` (`mkdisk.py`; sparse images are expanded) |
| `android.initrd` | vendor ramdisk + generic ramdisk + bootconfig trailer, as a boot image v4 bootloader would load them (`mkinitrd.py`) |
| `kernel` | the kernel `Image` out of `boot.img` |
| `android.json` | the platform: 2 harts, 4 GiB, 16550 UART, goldfish RTC, a PCIe host with virtio-blk (the disk), virtio-rng and virtio-gpu (720 x 1280) |
| `console.log` | the serial console |
| `screen.ppm` | the display when the run ends |

Extra arguments go to mvplat: `-max-cycles`, `-input-at S:TEXT` to type at
the shell at a simulated time, `-harts N` (or `HARTS=N`). The disk and
initrd are kept between runs; delete them to remake them.

`mkinitrd.py OUT - - key=value...` rewrites only the bootconfig of an
existing initrd, for trying a property.

## The board

Android's riscv64 kernels have no virtio-mmio driver built in, so the virtio
devices sit behind the `pcie` host (ECAM at 0x30000000) and the disk is found
by its PCI path (`androidboot.boot_devices`). The PLIC has 95 sources, as
QEMU's `virt` board: Linux's `sifive-plic` driver writes past its saved
enable words when the count is a multiple of 32.

What the bootconfig sets, besides the vendor_boot image's own entries:

| Entries | Why |
|---|---|
| `slot_suffix=_a`, `force_normal_boot=1`, `verifiedbootstate=orange` | what a bootloader passes (no AVB check is made) |
| `serialconsole=1`, `console=ttyS0` | a shell on the UART |
| `selinux=permissive` | Cuttlefish's default for userdebug |
| `boot_devices`, `fstab_suffix=cf.f2fs.hctr2` | the disk and the fstab for this build's f2fs userdata |
| `vendor.apex.*` gatekeeper and keymint `nonsecure` | no secure world |
| `hardware.gralloc=minigbm`, `hardware.hwcomposer=ranchu`, `hwcomposer.mode=guest`, `display_finder_mode=drm`, `egl=angle`, `vulkan=pastel` | software rendering on the virtio-gpu DRM device |
| `hw_timeout_multiplier=20` | a simulated CPU is slower than a phone: the framework's watchdogs wait 20 times longer |
| `vsock_lights_port=6900` | the lights HAL listens on a vsock port; without one it stops, and `system_server` waits for it until its watchdog restarts it |

## How far it gets

With `-harts 2 -mt` mvplat runs about 1 simulated second per second (145 to
185 million instructions per second).

| Simulated time | Stage |
|---|---|
| 0.5 s | OpenSBI, then the kernel |
| 17 s | first-stage init; the `super` partitions mapped through device-mapper, `system` verified (dm-verity) |
| 45 s | second-stage init, SELinux policy, apexd |
| 190 s | the serial shell (`console:/ $`) |
| 330 s | `/data` (f2fs) mounted, `odsign` (done at 417 s) |
| 520 s | `surfaceflinger` and the HWC3 composer up |
| 600 s | boot animation on screen |
| 900 s | zygote preloads, `system_server` starts its services |

The boot animation as mvplat's `virtio-gpu` shows it, `screen.ppm` at 3000 s:

![boot animation](images/android-bootanimation-riscv64.png)

Not yet:

- `sys.boot_completed` (the home screen) is not reached yet: `system_server`
  is the step being worked on.
- The Cuttlefish host services are not there, so the HALs that talk to them
  stop and are restarted: sensors, radio (RIL), Bluetooth, UWB, Thread, the
  OEM lock (`/dev/hvc10`), lights. None of them is needed to boot.
- No `adb` over the network (the virtio-net `user` back end has no NAT), no
  touch or keyboard input device, no audio.
- 3D: only CPU rendering (ANGLE on SwiftShader), no virgl.

The real target is AArch64 (phones, TVs); riscv64 comes first because
mvplat's RISC-V cores exist today.
