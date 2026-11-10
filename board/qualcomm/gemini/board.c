// SPDX-License-Identifier: GPL-2.0+
/* Xiaomi Mi 5: inherited hardware, standard extlinux parser and ARM64 booti. */
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/unaligned.h>
#include <blk.h>
#include <command.h>
#include <env.h>
#include <ext4fs.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <mapmem.h>
#include <part.h>
#include <pxe_utils.h>
#include <time.h>
#include <u-boot/sha256.h>

#include "gemini.h"
#include "panel.h"

DECLARE_GLOBAL_DATA_PTR;

/* qcom board_late_init reserves these buffers through the standard LMB API. */
#define GEMINI_KERNEL_LIMIT SZ_128M
#define GEMINI_INITRD_LIMIT SZ_128M
#define GEMINI_FDT_LIMIT SZ_2M
#define GEMINI_CONFIG_LIMIT SZ_64K
#define GEMINI_GZIP_LIMIT (12 * SZ_1M)
#define GEMINI_CONFIG "/boot/extlinux/extlinux.conf"

static struct {
	struct blk_desc *desc;
	char nonce[25];
	char config_hash[65];
	bool active, config_loaded, kernel_loaded, initrd_loaded, fdt_loaded;
} boot;

static int detected_panel;

#include "fdt.inc"

static const char *gemini_linux_panel(const void *fdt)
{
	bool jdi = fdt_node_offset_by_compatible(fdt, -1, "jdi,fhd-r63452") >= 0;
	bool lgd = fdt_node_offset_by_compatible(fdt, -1, "lgd,fhd-td4322") >= 0;

	if (jdi == lgd)
		return NULL;
	return jdi ? "jdi" : "lgd";
}

static int gemini_nonce(void)
{
	static const char key[] = "gemini_uboot_autorun=";
	const char *args, *end, *word;
	int chosen, len, count = 0;
	u32 size;

	chosen = fdt_path_offset(gd->fdt_blob, "/chosen");
	args = fdt_getprop(gd->fdt_blob, chosen, "bootargs", &len);
	if (!args || len < 1 || len > 2048 || args[len - 1])
		return -EINVAL;
	while (*args) {
		while (*args == ' ')
			args++;
		word = args;
		end = strchr(args, ' ');
		size = end ? end - args : strlen(args);
		args += size;
		if (size < sizeof(key) - 1 || memcmp(word, key, sizeof(key) - 1))
			continue;
		if (++count != 1 || size != sizeof(key) - 1 + 24)
			return -EINVAL;
		word += sizeof(key) - 1;
		for (int n = 0; n < 24; n++)
			if (!((word[n] >= '0' && word[n] <= '9') ||
			      (word[n] >= 'a' && word[n] <= 'f')))
				return -EINVAL;
		memcpy(boot.nonce, word, 24);
		boot.nonce[24] = 0;
	}
	if (!count)
		snprintf(boot.nonce, sizeof(boot.nonce), "%016llx%08x",
			 (unsigned long long)get_ticks(), (u32)get_timer(0));
	return 0;
}

static int gemini_firmware_panel(const void *fdt)
{
	const char *args, *marker;
	int chosen, len;

	marker = fdt_getprop(fdt, 0, "u-boot,gemini-input", &len);
	if (!marker || len != sizeof("universal-v1") ||
	    memcmp(marker, "universal-v1", len))
		return -EINVAL;
	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return chosen;
	args = fdt_getprop(fdt, chosen, "bootargs", &len);
	if (!args || len < 1)
		return -EINVAL;
	return gemini_panel_from_bootargs(args, len);
}

void qcom_late_init(void)
{
	const char *panel;
	char path[80];

	detected_panel = gemini_firmware_panel(gd->fdt_blob);
	panel = gemini_panel_name(detected_panel);
	if (!panel)
		panic("Mi 5: firmware panel detection failed (%d)\n", detected_panel);
	printf("Mi 5 universal-v1: aboot detected %s panel\n", panel);
	snprintf(path, sizeof(path), "msm8996-xiaomi-gemini-%s.dtb", panel);
	if (env_set("fdtfile", path))
		panic("Mi 5: cannot set fdtfile\n");
	/* Let standard bootm move FDT/initrd using LMB, without a 32-bit ceiling. */
	if (env_set("fdt_high", NULL) || env_set("initrd_high", NULL))
		panic("Mi 5: cannot configure boot memory\n");
}

static int gemini_args(void *fdt)
{
	char args[4096], suffix[384];
	const char *src, *end, *base = env_get("bootargs");
	int len, chosen;
	size_t size, used;

	if (!base || strlcpy(args, base, sizeof(args)) >= sizeof(args))
		return -EINVAL;
	/* Keep firmware identifiers and active-panel data, preserving APPEND. */
	chosen = fdt_path_offset(gd->fdt_blob, "/chosen");
	src = fdt_getprop(gd->fdt_blob, chosen, "bootargs", &len);
	if (!src || len < 1 || len > 2048 || src[len - 1])
		return -EINVAL;
	while (*src) {
		while (*src == ' ')
			src++;
		end = strchr(src, ' ');
		size = end ? end - src : strlen(src);
		if ((size >= 12 && !memcmp(src, "androidboot.", 12)) ||
		    (size >= 15 && !memcmp(src, "mdss_mdp.panel=", 15))) {
			used = strlen(args);
			if (used + size + 2 > sizeof(args))
				return -E2BIG;
			args[used++] = ' ';
			memcpy(args + used, src, size);
			args[used + size] = 0;
		}
		src += size;
	}
	snprintf(suffix, sizeof(suffix),
		 " gemini_uboot_handoff=%s gemini_uboot_source=ext4"
		 " gemini_uboot_autoboot=1 gemini_uboot_flow=extlinux"
		 " gemini_uboot_config=%s gemini_uboot_input=universal-v1"
		 " gemini_uboot_panel=%s", boot.nonce, boot.config_hash,
		 gemini_panel_name(detected_panel));
	if (strlcat(args, suffix, sizeof(args)) >= sizeof(args))
		return -E2BIG;
	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return chosen;
	return fdt_setprop_string(fdt, chosen, "bootargs", args);
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	const char *panel = gemini_linux_panel(blob);
	const char *runtime_panel = gemini_panel_name(detected_panel);

	if (!boot.active || !boot.config_loaded || !boot.kernel_loaded ||
	    !boot.initrd_loaded || !boot.fdt_loaded || !panel || !runtime_panel ||
	    strcmp(panel, runtime_panel))
		return -EINVAL;
	return gemini_args(blob);
}

static int gemini_usb_idle(void)
{
	static const u32 branches[] = { 0x0f008, 0x0f00c, 0x0f010, 0x0f03c, 0x6a004 };
	const ulong gcc = 0x00300000UL, usb = 0x06a00000UL;
	u32 reg;

	reg = readl((void *)(gcc + 0x0f004));
	if (!(reg & BIT(31)) || (reg & BIT(0)) ||
	    (readl((void *)(gcc + 0x0f000)) & BIT(0)) ||
	    (readl((void *)(gcc + 0x12038)) & BIT(0)) ||
	    (readl((void *)(gcc + 0x6a000)) & BIT(0)))
		return -EHOSTDOWN;
	for (int i = 0; i < ARRAY_SIZE(branches); i++) {
		reg = readl((void *)(gcc + branches[i]));
		if (!(reg & BIT(0)) || (reg & BIT(31)))
			return -EHOSTDOWN;
	}
	/* DWC3 GSNPSID/GCTL/DCTL/DSTS, no gadget is started by this port. */
	if (readl((void *)(usb + 0xc120)) != 0x5533270a ||
	    ((readl((void *)(usb + 0xc110)) >> 12) & 3) != 2 ||
	    (readl((void *)(usb + 0xc704)) & BIT(31)) ||
	    !(readl((void *)(usb + 0xc70c)) & BIT(22)))
		return -EBUSY;
	return 0;
}

void board_quiesce_devices(void)
{
	int ret;

	if (!boot.active)
		panic("Mi 5: no active extlinux boot\n");
	ret = gemini_ufs_quiesce();
	if (!ret)
		ret = gemini_usb_idle();
	if (ret)
		panic("Mi 5: hardware handoff failed (%d)\n", ret);
	puts("Mi 5: UFS halted, USB idle; entering Linux through booti\n");
	asm volatile("msr daifset, #0xf" : : : "memory");
}

static bool gemini_path(const char *path)
{
	size_t len = strlen(path);

	if (len < 7 || len > 255 || strncmp(path, "/boot/", 6) || strstr(path, ".."))
		return false;
	for (int i = 6; path[i]; i++)
		if (!((path[i] >= 'a' && path[i] <= 'z') ||
		      (path[i] >= 'A' && path[i] <= 'Z') ||
		      (path[i] >= '0' && path[i] <= '9') ||
		      path[i] == '/' || path[i] == '.' || path[i] == '_' || path[i] == '-'))
			return false;
	return true;
}

/* The same parser/loader contract used by bootmeth_extlinux and sysboot.
 * Only storage access is board-specific. Neither grammar nor kernel entry
 * is reimplemented here. Bounds cover the buffers reserved by Qualcomm init.
 */
static int gemini_getfile(struct pxe_context *ctx, const char *path,
			  char *address, enum bootflow_img_t type, ulong *sizep)
{
	struct disk_partition part = { 0 };
	loff_t size, actual;
	ulong addr, expected;
	u32 limit;
	u8 hash[32];
	void *buffer;
	int ret;

	if (!boot.active || !boot.desc || !gemini_path(path) ||
	    strict_strtoul(address, 16, &addr))
		return -EINVAL;
	switch ((int)type) {
	case BFI_EXTLINUX_CFG:
		expected = env_get_hex("pxefile_addr_r", 0);
		limit = GEMINI_CONFIG_LIMIT - 1;
		/* Includes need extra destination ranges; keep the initial port bounded. */
		if (strcmp(path, GEMINI_CONFIG) || boot.config_loaded)
			return -ENOTSUPP;
		break;
	case IH_TYPE_KERNEL:
		expected = env_get_hex("kernel_addr_r", 0);
		limit = GEMINI_KERNEL_LIMIT;
		boot.kernel_loaded = false;
		break;
	case IH_TYPE_RAMDISK:
		expected = env_get_hex("ramdisk_addr_r", 0);
		limit = GEMINI_INITRD_LIMIT;
		boot.initrd_loaded = false;
		break;
	case IH_TYPE_FLATDT:
		expected = env_get_hex("fdt_addr_r", 0);
		limit = GEMINI_FDT_LIMIT;
		boot.fdt_loaded = false;
		break;
	default:
		return -ENOTSUPP;
	}
	if (!expected || addr != expected)
		return -ERANGE;
	buffer = map_sysmem(addr, limit);
	part.blksz = boot.desc->blksz;
	part.size = boot.desc->lba;
	ret = ext4fs_probe(boot.desc, &part);
	if (ret)
		goto out;
	ret = ext4fs_open(path, &size);
	if (ret || size < 1 || size > limit) {
		ret = ret ? ret : -EFBIG;
		goto out;
	}
	/* Small CPU reads use the transport's fixed low-address DMA bounce buffer. */
	for (loff_t offset = 0; offset < size; offset += actual) {
		loff_t chunk = min_t(loff_t, size - offset, SZ_64K);

		actual = 0;
		ret = ext4fs_read(buffer + offset, offset, chunk, &actual);
		if (ret || gemini_ufs_error() || actual != chunk) {
			ret = ret ? ret : -EIO;
			goto out;
		}
	}
	sha256_csum_wd(buffer, size, hash, SZ_64K);
	printf("Mi 5 file %s bytes=%llu sha256=", path, (unsigned long long)size);
	for (int i = 0; i < sizeof(hash); i++)
		printf("%02x", hash[i]);
	puts("\n");
	if (type == BFI_EXTLINUX_CFG) {
		for (int i = 0; i < sizeof(hash); i++)
			sprintf(boot.config_hash + i * 2, "%02x", hash[i]);
		boot.config_loaded = true;
	} else if ((int)type == IH_TYPE_KERNEL) {
		const u8 *kernel = buffer;

		if (size >= 2 && kernel[0] == 0x1f && kernel[1] == 0x8b) {
			/* Upstream booti permits 10 * compressed length. Reserve 128 MiB,
			 * accept gzip <=12 MiB so both decompression and copy-back fit.
			 */
			if (size > GEMINI_GZIP_LIMIT) {
				ret = -EFBIG;
				goto out;
			}
			ret = env_set_hex("kernel_comp_size", size);
		} else if (size < 64 || get_unaligned_le32(kernel + 56) != 0x644d5241) {
			ret = -EINVAL;
		}
		boot.kernel_loaded = !ret;
	} else if ((int)type == IH_TYPE_RAMDISK) {
		boot.initrd_loaded = true;
	} else {
		const char *panel, *runtime_panel;

		if (fdt_check_full(buffer, size) || fdt_totalsize(buffer) != size ||
		    fdt_node_check_compatible(buffer, 0, "xiaomi,gemini") ||
		    fdt_node_check_compatible(buffer, 0, "qcom,msm8996")) {
			ret = -EINVAL;
			goto out;
		}
		panel = gemini_linux_panel(buffer);
		runtime_panel = gemini_panel_name(detected_panel);
		if (!panel || !runtime_panel || strcmp(panel, runtime_panel)) {
			ret = -EINVAL;
			goto out;
		}
		/* Standard bootm consumes FDT reservations before ft_board_setup.
		 * Apply firmware ranges now, before it relocates FDT or initrd.
		 */
		ret = fdt_open_into(buffer, buffer, GEMINI_FDT_LIMIT);
		if (!ret)
			ret = gb_fdt_fixups(buffer, gd->fdt_blob);
		if (!ret)
			ret = fdt_pack(buffer);
		if (ret)
			goto out;
		boot.fdt_loaded = true;
	}
	if (!ret)
		*sizep = size;
out:
	ext4fs_close();
	unmap_sysmem(buffer);
	if (gemini_ufs_error())
		ret = gemini_ufs_error();
	return ret;
}

static int do_gemini_boot(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	struct pxe_context ctx;
	ulong addr = env_get_hex("pxefile_addr_r", 0);
	int ret, cleanup;
	bool setup = false;

	if (boot.active || !addr || (current_el() != 1 && current_el() != 2))
		return CMD_RET_FAILURE;
	memset(&boot, 0, sizeof(boot));
	ret = gemini_nonce();
	if (ret)
		goto fail;
	ret = gemini_ufs_open(&boot.desc);
	if (ret)
		goto fail;
	boot.active = true;
	ret = pxe_setup_ctx(&ctx, cmdtp, gemini_getfile, NULL, true,
			    GEMINI_CONFIG, false, false);
	if (ret)
		goto fail;
	setup = true;
	puts("Mi 5: upstream extlinux -> booti\n");
	ret = get_pxe_file(&ctx, GEMINI_CONFIG, addr);
	if (ret >= 0)
		ret = pxe_process(&ctx, addr, false);
fail:
	if (setup)
		pxe_destroy_ctx(&ctx);
	cleanup = gemini_ufs_close(true);
	boot.active = false;
	printf("Mi 5 extlinux boot returned: %d cleanup=%d\n", ret, cleanup);
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(gemini_boot, 1, 0, do_gemini_boot,
	   "boot /boot/extlinux/extlinux.conf on inherited read-only UFS", "");
