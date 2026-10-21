# pipa for UniXNU

> **Work in progress.** Prepared without the tablet; nothing here has run on a
> Xiaomi Pad 6 yet.

This branch adds UniXNU to the Mu-Silicium pipa port (Kona, SM8250). It is
not hardware validated: nothing of it has run on a Xiaomi Pad 6 yet. The board
file, the ADT generator and the build steps are in the
[UniXNU-pipa](https://github.com/xVanTuring/UniXNU-pipa) device repository.

## Changes

* `pipa.dsc` builds UniXNU, its registrar UniXNUDxe, the ramdisk block device
  driver and DarwinBoot, and maps UniXNULib and FdtLib.
* `pipa.fdf` places UniXNU and UniXNUDxe in the firmware volume.
* `Include/DXE.inc` adds `shengPkg/Drivers/ShengBootMemoryDxe` and DarwinBoot.
  The memory driver reads the device tree the previous stage passes in x0
  (kept by SEC in a HOB) and publishes the boot image's ramdisk, the UniXNU
  payload volume, as a read-only block device. It takes every range from the
  device tree, so it serves this board without a copy. It also adds DRAM the
  GCD does not know yet, below 4 GiB and up to 1 GiB; RamManagerDxe maps only
  what the memory map leaves free, so the order of the two does not matter.
  DarwinBoot is part of the firmware volume because this build's image
  verification refuses a driver loaded from a volume.
* `Resources/Configs/pipa.toml` writes out the boot image base, kernel and tags
  offsets and page size that the image has always been packed with (the
  mkbootimg defaults), because the UniXNU driver applies different defaults.

The upstream vendor driver set, `USE_CUSTOM_DISPLAY_DRIVER = 0` (SimpleFbDxe
on the frame buffer ABL leaves scanning out) and the memory map are unchanged.
`Resources/DTBs/pipa.dtb` carries `/reserved-memory/cont_splash_region` at
0x9c000000, inside the "Display Reserved" descriptor.

## Unverified

* Whether the firmware volume still fits the 3 MiB FD with UniXNU added. The
  vendor inputs are 4.8 MiB before compression.
* Whether the panel stays on through the vendor clock and power drivers when
  UniXNU's menu runs (on sheng the same stack blanked it).
* Whether ABL places a 192 MiB ramdisk of a header v1 image and reports it in
  `/chosen` `linux,initrd-start` and `linux,initrd-end`.
* Whether `Resources/MuPatches/Gic-Spurious.patch`, applied by the UniXNU
  build, is needed here; it masks an interrupt that has no handler.
