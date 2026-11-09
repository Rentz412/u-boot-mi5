# U-Boot for Xiaomi Mi 5 (gemini)

This is a second-stage U-Boot port for the Xiaomi Mi 5 (MSM8996), chainloaded
from the stock Android bootloader (`aboot`).

Rather than replacing XBL or re-initializing the hardware from scratch, U-Boot
piggybacks on the state left behind by the stock firmware for display (MDP),
storage (UFS/UniPro), and USB/PHY. Once flashed, U-Boot lives inside the
`boot` partition as an Android boot image, while the actual Linux kernel,
initramfs, and DTBs live on an ext4 filesystem inside `userdata` — so you can
update kernels and `/boot/extlinux/extlinux.conf` directly from Linux without
reflashing the boot partition every time.

## Building

Build out-of-tree using the standard U-Boot build system:

```sh
make O=../build-mi5 CROSS_COMPILE=aarch64-linux-gnu- qcom_gemini_defconfig
make O=../build-mi5 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) u-boot-nodtb.bin
```

### Packaging the boot image

The stock bootloader expects a legacy Android v0 boot image with a board-specific
DTB appended. Use `mkimage.py` (alongside AOSP's `mkbootimg.py` and the matching
firmware input DTBs) to wrap `u-boot-nodtb.bin`.

Because runtime panel auto-detection isn't implemented yet, JDI and LGD panels
need separate U-Boot images built with their respective input DTBs. The runtime
DTB baked into U-Boot determines `fdtfile`:
- `msm8996-xiaomi-gemini-jdi.dtb`
- `msm8996-xiaomi-gemini-lgd.dtb`

## How Booting Works (`gemini_boot`)

Instead of exposing a full generic SCSI/UFS stack or wiring up `bootflow scan`,
`gemini_boot` implements a minimal read-only wrapper around the UFS controller
state inherited from `aboot`:

1. **Storage:** Validates GPT (with CRC checks) on UFS LU0 and exposes the
   `userdata` partition as a read-only DM block device to U-Boot's ext4 driver.
2. **Parsing:** Hands off to upstream's standard extlinux parser
   (`pxe_setup_ctx` / `get_pxe_file` / `pxe_process` — the same code path used
   by `sysboot` and `bootmeth_extlinux`) to read `/boot/extlinux/extlinux.conf`.
3. **Device Tree Fixups:** Forward-ports memory and reserved-memory nodes from
   the firmware prior to `bootm`, while `ft_board_setup` preserves the stock
   firmware IDs and records boot-source metadata.
4. **Handoff:** Before jumping to Linux via upstream `booti` (which handles
   decompression and relocation), `board_quiesce_devices` stops UFS DMA and
   checks that the inherited USB controller is idle.

If booting fails for any reason, U-Boot falls back to the default environment
and triggers a reset after 30 seconds. Neither USB gadget mode nor interactive
console input is enabled in this build.

## Boot Configuration (`extlinux.conf`)

Create `/boot/extlinux/extlinux.conf` on the `userdata` ext4 partition. Standard
directives (`LINUX`, `INITRD`, `FDTDIR`, `APPEND`) and multiple menu labels are
supported.

A typical setup uses `FDTDIR /boot/gemini`, which automatically resolves to
`msm8996-xiaomi-gemini-jdi.dtb` or `msm8996-xiaomi-gemini-lgd.dtb` based on
`fdtfile`. Make sure the kernel DTB on disk matches your panel variant.

### Current Limitations & Requirements

- **Filesystem:** Read-only; journal replay is not supported. The `userdata`
  ext4 filesystem must be cleanly unmounted, use 4 KiB blocks, and stick to
  ext4 feature flags supported by U-Boot.
- **Paths & directives:** All boot files (`kernel`, `initramfs`, and `DTB` are
  all mandatory) must reside under `/boot` without `..` path traversal or shell
  metacharacters. `INCLUDE`, DT overlays, and boot logos are not supported.
- **Size & memory limits:**
  - Qualcomm board init reserves fixed buffers: **128 MiB** for the kernel,
    **128 MiB** for the initramfs, **128 MiB** for decompression, and **2 MiB**
    for the DTB.
  - Uncompressed `Image` kernels can be up to 128 MiB. Gzip-compressed
    `Image.gz` kernels are capped at **12 MiB** so upstream `booti`'s 10x
    decompression estimate fits comfortably within the target buffers.
  - Total UFS read volume per boot is capped at 128 MiB (with a 120s timeout).
  - Only the UFS DMA bounce buffer is restricted to the lower 4 GiB of physical
    RAM; file buffers accessed by the CPU can sit above 4 GiB.
