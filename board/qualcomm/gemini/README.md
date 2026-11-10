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
input DTB appended. The packer now creates a single `u-boot-mi5.img` for both
JDI and LGD panels. Pass a known-good Mi 5 firmware input DTB (either panel),
or a previously generated common input DTB:

```sh
python3 board/qualcomm/gemini/mkimage.py \
    --uboot ../build-mi5/u-boot-nodtb.bin \
    --input-dtb /path/to/gemini-input.dtb \
    --mkbootimg /path/to/mkbootimg.py --output ../images-mi5
```

`dtc` and `fdtget` must be installed. The packer preserves the known input tree's
Qualcomm matching IDs and hardware layout, replacing its fixed Linux panel
compatible with a neutral control-tree marker. It never modifies the input file.
Generated `input-dtc.log` records diagnostics inherited from the supplied tree.

U-Boot reads the stock aboot `mdss_mdp.panel=` argument from the external FDT
and selects `msm8996-xiaomi-gemini-jdi.dtb` or
`msm8996-xiaomi-gemini-lgd.dtb` through `fdtfile`. The initial allowlist contains
the exact `qcom,mdss_dsi_jdi_fhd_r63452_cmd` and
`qcom,mdss_dsi_lgd_fhd_td4322_cmd` names, with `1:dsi:0:` initialized-panel
prefix. Missing, repeated, malformed or unknown panel parameters stop boot;
they do not silently select a default. Other JDI naming variants need evidence
before they can be added. Loaded Linux DTBs must match the detected panel.

This input control DTB is for U-Boot, not for direct Linux boot. Linux still
uses separate panel DTBs stored on ext4. Touchscreen variants are independent:
select a DTB directory matching the actual touch controller, with both panel
filenames inside it. The loader does not auto-detect touch controllers.

The common-input image has passed a temporary `fastboot boot` test on the JDI
device with the existing 6.3.1 Ubuntu kernel, verified using an exact boot nonce,
fresh Linux boot ID, correct Linux panel compatible and functioning USB serial.
LGD parameters have been exercised in host tests; LGD hardware remains untested.
This experimental common version has not been installed permanently.

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
`fdtfile` and the panel reported by aboot. Make sure the kernel DTB on disk matches your panel variant.

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
