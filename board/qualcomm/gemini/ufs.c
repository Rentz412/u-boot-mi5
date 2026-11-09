// SPDX-License-Identifier: GPL-2.0+
/* Read-only MSM8996 UFS transport using the link initialized by stock firmware. */
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/unaligned.h>
#include <blk.h>
#include <cpu_func.h>
#include <dm.h>
#include <dm/root.h>
#include <dm/device-internal.h>
#include <ext4fs.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <lmb.h>
#include <part.h>
#include <time.h>
#include <u-boot/crc.h>
#include <ufs/ufs.h>
#include "gemini.h"

#define GEMINI_GCC 0x00300000UL
#define GEMINI_UFS 0x00624000UL
#define GEMINI_DMA_SIZE 0x20000
#define GEMINI_DATA_OFFSET 0x4000
#define GEMINI_DATA_SIZE 0x10000
#define GEMINI_UCD_OFFSET 0x1000

/* Kept until reset if a timed-out transfer might still own the DMA region. */
static bool gemini_ufs_probe_blocked;

static u32 gp_read(u32 offset)
{
	return readl((void *)(GEMINI_UFS + offset));
}

static void gp_write(u32 value, u32 offset)
{
	writel(value, (void *)(GEMINI_UFS + offset));
}

static int gp_guard(void)
{
	static const u32 branches[] = {
		0x75008, 0x7500c, 0x75038, 0x83014, 0x7600c,
	};
	u32 value;
	int i;

	if (!of_machine_is_compatible("xiaomi,gemini") ||
	    !of_machine_is_compatible("qcom,msm8996"))
		return -ENODEV;
	value = readl((void *)(GEMINI_GCC + 0x75004));
	if (!(value & BIT(31)) || (value & BIT(0)) ||
	    (readl((void *)(GEMINI_GCC + 0x75000)) & BIT(0)))
		return -EHOSTDOWN;
	for (i = 0; i < ARRAY_SIZE(branches); i++) {
		value = readl((void *)(GEMINI_GCC + branches[i]));
		if (!(value & BIT(0)) || (value & BIT(31)))
			return -EHOSTDOWN;
	}
	/* Restrict this first active probe to the exact captured controller. */
	if (gp_read(REG_UFS_VERSION) != UFSHCI_VERSION_11 ||
	    gp_read(0xe4) != 0x20020000)
		return -ENODEV;
	value = gp_read(REG_CONTROLLER_STATUS);
	if (!(gp_read(REG_CONTROLLER_ENABLE) & CONTROLLER_ENABLE) ||
	    (value & (DEVICE_PRESENT | UFSHCD_STATUS_READY)) !=
	    (DEVICE_PRESENT | UFSHCD_STATUS_READY) ||
	    (value & (HOST_ERROR_INDICATOR | DEVICE_ERROR_INDICATOR)))
		return -EIO;
	if (gp_read(REG_UTP_TRANSFER_REQ_DOOR_BELL) ||
	    gp_read(REG_UTP_TASK_REQ_DOOR_BELL))
		return -EBUSY;
	if (gp_read(REG_INTERRUPT_STATUS) & UFSHCD_ERROR_MASK)
		return -EIO;
	return 0;
}

static int gp_wait(u32 offset, u32 mask, u32 expected, ulong timeout_ms)
{
	ulong start = get_timer(0);

	while ((gp_read(offset) & mask) != expected) {
		if (get_timer(start) >= timeout_ms)
			return -ETIMEDOUT;
		udelay(100);
	}
	return 0;
}

static int gp_dme_get(u16 attr, u32 *value)
{
	u32 status;
	int ret;

	if (!(gp_read(REG_CONTROLLER_STATUS) & UIC_COMMAND_READY))
		return -EBUSY;
	/* Clear only the stale UIC completion, preserving other diagnostics. */
	gp_write(UIC_COMMAND_COMPL, REG_INTERRUPT_STATUS);
	gp_write(UIC_ARG_MIB(attr), REG_UIC_COMMAND_ARG_1);
	gp_write(0, REG_UIC_COMMAND_ARG_2);
	gp_write(0, REG_UIC_COMMAND_ARG_3);
	wmb();
	gp_write(UIC_CMD_DME_GET, REG_UIC_COMMAND);
	ret = gp_wait(REG_INTERRUPT_STATUS, UIC_COMMAND_COMPL,
		      UIC_COMMAND_COMPL, 1000);
	status = gp_read(REG_INTERRUPT_STATUS);
	printf("UFS UIC attr=%04x wait=%d IS=%08x ARG2=%08x\n", attr,
	       ret, status, gp_read(REG_UIC_COMMAND_ARG_2));
	if (ret || (status & UFSHCD_ERROR_MASK))
		return ret ? ret : -EIO;
	ret = gp_read(REG_UIC_COMMAND_ARG_2) & MASK_UIC_COMMAND_RESULT;
	*value = gp_read(REG_UIC_COMMAND_ARG_3);
	gp_write(UIC_COMMAND_COMPL, REG_INTERRUPT_STATUS);
	return ret ? -EIO : 0;
}

/* The only accepted requests are NOP and standard READ_DESCRIPTOR (device). */
static int gp_prepare(struct utp_transfer_req_desc *trd,
		      struct utp_transfer_cmd_desc *ucd, bool descriptor)
{
	struct utp_upiu_req *req = (void *)ucd->command_upiu;
	ulong ucd_addr = (ulong)ucd;

	memset(trd, 0, 1024);
	memset(ucd, 0, sizeof(*ucd));
	trd->header.dword_0 = cpu_to_le32(UTP_NATIVE_UFS_COMMAND |
					UTP_REQ_DESC_INT_CMD);
	trd->header.dword_2 = cpu_to_le32(OCS_INVALID_COMMAND_STATUS);
	trd->command_desc_base_addr_lo = cpu_to_le32(lower_32_bits(ucd_addr));
	trd->command_desc_base_addr_hi = cpu_to_le32(upper_32_bits(ucd_addr));
	trd->response_upiu_offset = cpu_to_le16(offsetof(
		struct utp_transfer_cmd_desc, response_upiu) / 4);
	trd->response_upiu_length = cpu_to_le16(ALIGNED_UPIU_SIZE / 4);
	trd->prd_table_offset = cpu_to_le16(offsetof(
		struct utp_transfer_cmd_desc, prd_table) / 4);
	if (descriptor) {
		req->header.dword_0 = UPIU_HEADER_DWORD(
			UPIU_TRANSACTION_QUERY_REQ, 0, 0, 0);
		req->header.dword_1 = UPIU_HEADER_DWORD(0,
			UPIU_QUERY_FUNC_STANDARD_READ_REQUEST, 0, 0);
		req->qr.opcode = UPIU_QUERY_OPCODE_READ_DESC;
		req->qr.idn = QUERY_DESC_IDN_DEVICE;
		req->qr.length = cpu_to_be16(QUERY_DESC_DEVICE_DEF_SIZE);
	}
	/* NOP_OUT is all-zero UPIU. Neither request has data-out or PRDT. */
	flush_dcache_range((ulong)trd, (ulong)trd + 1024);
	flush_dcache_range(ucd_addr, ucd_addr + sizeof(*ucd));
	return 0;
}

static int gp_exchange(struct utp_transfer_req_desc *trd,
		       struct utp_transfer_cmd_desc *ucd, bool descriptor)
{
	struct utp_upiu_rsp *rsp = (void *)ucd->response_upiu;
	u32 status, ocs, hdr0, hdr1;
	int ret;

	if (gp_read(REG_UTP_TRANSFER_REQ_DOOR_BELL))
		return -EBUSY;
	gp_prepare(trd, ucd, descriptor);
	gp_write(UTP_TRANSFER_REQ_COMPL, REG_INTERRUPT_STATUS);
	wmb();
	gp_write(1, REG_UTP_TRANSFER_REQ_DOOR_BELL);
	ret = gp_wait(REG_UTP_TRANSFER_REQ_DOOR_BELL, 1, 0, 1500);
	status = gp_read(REG_INTERRUPT_STATUS);
	invalidate_dcache_range((ulong)trd, (ulong)trd + 1024);
	invalidate_dcache_range((ulong)ucd, (ulong)ucd + sizeof(*ucd));
	ocs = le32_to_cpu(trd->header.dword_2) & MASK_OCS;
	hdr0 = be32_to_cpu(rsp->header.dword_0);
	hdr1 = be32_to_cpu(rsp->header.dword_1);
	printf("UFS XFER %s ret=%d IS=%08x DBR=%08x OCS=%x RSP=%08x/%08x\n",
	       descriptor ? "READ_DESC" : "NOP", ret, status,
	       gp_read(REG_UTP_TRANSFER_REQ_DOOR_BELL), ocs, hdr0, hdr1);
	if (ret)
		return ret;
	if ((status & UFSHCD_ERROR_MASK) || ocs || (hdr0 & 0xffff) ||
	    (hdr1 & MASK_RSP_UPIU_RESULT) ||
	    (hdr0 >> 24) != (descriptor ? UPIU_TRANSACTION_QUERY_RSP :
					 UPIU_TRANSACTION_NOP_IN))
		return -EIO;
	gp_write(UTP_TRANSFER_REQ_COMPL, REG_INTERRUPT_STATUS);
	return 0;
}

static int gp_device_desc(struct utp_transfer_cmd_desc *ucd)
{
	struct utp_upiu_rsp *rsp = (void *)ucd->response_upiu;
	u8 *desc = ucd->response_upiu + GENERAL_UPIU_REQUEST_SIZE;
	u16 len = be32_to_cpu(rsp->header.dword_2) & MASK_QUERY_DATA_SEG_LEN;

	if (rsp->qr.opcode != UPIU_QUERY_OPCODE_READ_DESC ||
	    rsp->qr.idn != QUERY_DESC_IDN_DEVICE || rsp->qr.index ||
	    rsp->qr.selector || len < 0x1a || len > QUERY_DESC_DEVICE_DEF_SIZE ||
	    be16_to_cpu(rsp->qr.length) != len ||
	    desc[0] < len || desc[1] != QUERY_DESC_IDN_DEVICE)
		return -EINVAL;
	printf("UFS DEVICE_DESC len=%u manufacturer=%02x%02x spec=%02x%02x luns=%u\n",
	       len, desc[0x18], desc[0x19], desc[0x10], desc[0x11], desc[6]);
	return 0;
}

/* The only SCSI opcodes accepted by this diagnostic are CAPACITY(10)/READ(10). */
static int gp_scsi(struct utp_transfer_req_desc *trd,
		   struct utp_transfer_cmd_desc *ucd, u8 lun, u8 opcode,
		   u32 lba, u16 blocks, u32 block_size)
{
	struct utp_upiu_req *req = (void *)ucd->command_upiu;
	struct utp_upiu_rsp *rsp = (void *)ucd->response_upiu;
	u8 *data = (u8 *)trd + GEMINI_DATA_OFFSET;
	u32 bytes, status, hdr0, hdr1, ocs, residual, segment_len, response;
	u16 sense_len;
	u8 key = 0, asc = 0, ascq = 0, code;
	int ret;

	if (lun >= 8 || (opcode != 0x25 && opcode != 0x28))
		return -EINVAL;
	if (opcode == 0x25) {
		if (lba || blocks || block_size)
			return -EINVAL;
		bytes = 8;
	} else {
		if (!blocks || (block_size != 512 && block_size != 4096) ||
		    blocks > GEMINI_DATA_SIZE / block_size ||
		    (u64)lba + blocks > 0x100000000ULL)
			return -EINVAL;
		bytes = blocks * block_size;
	}
	if (gp_read(REG_UTP_TRANSFER_REQ_DOOR_BELL))
		return -EBUSY;
	gp_prepare(trd, ucd, false);
	memset(data, 0, GEMINI_DATA_SIZE);
	trd->header.dword_0 = cpu_to_le32(UTP_NATIVE_UFS_COMMAND |
		UTP_REQ_DESC_INT_CMD | (UTP_DEVICE_TO_HOST << UPIU_DATA_DIRECTION_OFFSET));
	trd->prd_table_length = cpu_to_le16(1);
	ucd->prd_table[0].base_addr = cpu_to_le32(lower_32_bits((ulong)data));
	ucd->prd_table[0].upper_addr = 0;
	ucd->prd_table[0].size = cpu_to_le32(bytes - 1);
	req->header.dword_0 = UPIU_HEADER_DWORD(UPIU_TRANSACTION_COMMAND,
						UPIU_CMD_FLAGS_READ, lun, 0);
	req->sc.exp_data_transfer_len = cpu_to_be32(bytes);
	req->sc.cdb[0] = opcode;
	if (opcode == 0x28) {
		req->sc.cdb[2] = lba >> 24;
		req->sc.cdb[3] = lba >> 16;
		req->sc.cdb[4] = lba >> 8;
		req->sc.cdb[5] = lba;
		req->sc.cdb[7] = blocks >> 8;
		req->sc.cdb[8] = blocks;
	}
	flush_dcache_range((ulong)trd, (ulong)trd + 1024);
	flush_dcache_range((ulong)ucd, (ulong)ucd + sizeof(*ucd));
	flush_dcache_range((ulong)data, (ulong)data + GEMINI_DATA_SIZE);
	gp_write(UTP_TRANSFER_REQ_COMPL, REG_INTERRUPT_STATUS);
	wmb();
	gp_write(1, REG_UTP_TRANSFER_REQ_DOOR_BELL);
	ret = gp_wait(REG_UTP_TRANSFER_REQ_DOOR_BELL, 1, 0, 1500);
	status = gp_read(REG_INTERRUPT_STATUS);
	invalidate_dcache_range((ulong)trd, (ulong)trd + 1024);
	invalidate_dcache_range((ulong)ucd, (ulong)ucd + sizeof(*ucd));
	invalidate_dcache_range((ulong)data, (ulong)data + GEMINI_DATA_SIZE);
	ocs = le32_to_cpu(trd->header.dword_2) & MASK_OCS;
	hdr0 = be32_to_cpu(rsp->header.dword_0);
	hdr1 = be32_to_cpu(rsp->header.dword_1);
	if (ret || ocs || (hdr1 & 0xffff))
	printf("UFS SCSI lun=%u op=%02x lba=%u blocks=%u ret=%d IS=%08x OCS=%x RSP=%08x/%08x\n",
	       lun, opcode, lba, blocks, ret, status, ocs, hdr0, hdr1);
	if (ret)
		return ret;
	if ((status & UFSHCD_ERROR_MASK) || ocs ||
	    (hdr0 >> 24) != UPIU_TRANSACTION_RESPONSE ||
	    (hdr0 & 0xffff) != ((u32)lun << 8))
		return -EIO;
	gp_write(UTP_TRANSFER_REQ_COMPL, REG_INTERRUPT_STATUS);
	if ((hdr1 & 0xff) == 2) {
		/* A device CHECK CONDITION can carry response=1 (captured on LU6).
		 * Decode its bounded sense before classifying the failure. Never
		 * treat nonzero response with GOOD status as successful data.
		 */
		response = (hdr1 >> 8) & 0xff;
		segment_len = be32_to_cpu(rsp->header.dword_2) & 0xffff;
		sense_len = be16_to_cpu(rsp->sr.sense_data_len);
		printf("UFS CHECK lun=%u response=%02x segment_len=%u sense_len=%u raw=",
		       lun, response, segment_len, sense_len);
		for (int n = 0; n < RESPONSE_UPIU_SENSE_DATA_LENGTH; n++)
			printf("%02x", rsp->sr.sense_data[n]);
		puts("\n");
		if (response > 1 || sense_len > RESPONSE_UPIU_SENSE_DATA_LENGTH ||
		    segment_len < 2U + sense_len || segment_len > 20)
			return -EIO;
		code = rsp->sr.sense_data[0] & 0x7f;
		if (sense_len >= 14 && (code == 0x70 || code == 0x71)) {
			key = rsp->sr.sense_data[2] & 15;
			asc = rsp->sr.sense_data[12];
			ascq = rsp->sr.sense_data[13];
		} else if (sense_len >= 4 && (code == 0x72 || code == 0x73)) {
			key = rsp->sr.sense_data[1] & 15;
			asc = rsp->sr.sense_data[2];
			ascq = rsp->sr.sense_data[3];
		}
		printf("UFS SENSE lun=%u key=%x asc=%02x ascq=%02x\n",
		       lun, key, asc, ascq);
		if (key == 6)
			return -EAGAIN;
		if (opcode == 0x25 && ascq == 0 &&
		    ((key == 5 && asc == 0x25) || (key == 2 && asc == 0x3a)))
			return -ENODEV;
		return -EIO;
	}
	residual = be32_to_cpu(rsp->sr.residual_transfer_count);
	if ((hdr1 & 0xffff) || residual || (hdr0 & 0x00ff0000))
		return -EIO;
	return 0;
}

static int gp_read_blocks(struct utp_transfer_req_desc *trd,
			  struct utp_transfer_cmd_desc *ucd, u8 lun,
			  u32 last_lba, u32 block_size, u32 lba, u16 blocks)
{
	if (!blocks || lba > last_lba || blocks - 1 > last_lba - lba)
		return -ERANGE;
	return gp_scsi(trd, ucd, lun, 0x28, lba, blocks, block_size);
}

struct gp_gpt_info {
	u64 first, last, table;
	u32 count, bytes, table_crc;
};

static int gp_gpt_header(u8 *data, u32 block_size, u32 last_lba,
			 struct gp_gpt_info *info)
{
	u32 size, saved_crc, computed, entry_size, table_blocks;

	if (memcmp(data, "EFI PART", 8))
		return -ENOENT;
	size = get_unaligned_le32(data + 12);
	if (size < 92 || size > block_size ||
	    get_unaligned_le32(data + 8) != 0x00010000 ||
	    get_unaligned_le32(data + 20))
		return -EINVAL;
	saved_crc = get_unaligned_le32(data + 16);
	memset(data + 16, 0, 4);
	computed = crc32(0, data, size);
	put_unaligned_le32(saved_crc, data + 16);
	if (saved_crc != computed)
		return -EBADMSG;
	info->first = get_unaligned_le64(data + 40);
	info->last = get_unaligned_le64(data + 48);
	info->table = get_unaligned_le64(data + 72);
	info->count = get_unaligned_le32(data + 80);
	entry_size = get_unaligned_le32(data + 84);
	info->table_crc = get_unaligned_le32(data + 88);
	if (get_unaligned_le64(data + 24) != 1 ||
	    get_unaligned_le64(data + 32) != last_lba ||
	    info->first > info->last || info->last >= last_lba ||
	    info->table < 2 || !info->count || info->count > 512 ||
	    entry_size != 128)
		return -EINVAL;
	info->bytes = info->count * entry_size;
	table_blocks = DIV_ROUND_UP(info->bytes, block_size);
	if (info->table > last_lba || table_blocks > last_lba - info->table ||
	    info->table + table_blocks > info->first)
		return -ERANGE;
	return 0;
}

static bool gp_entry_used(const u8 *entry)
{
	int i;

	for (i = 0; i < 16; i++)
		if (entry[i])
			return true;
	return false;
}

static int gp_gpt_entries(u8 lun, const u8 *data, const struct gp_gpt_info *info)
{
	u64 first, last, other_first, other_last;
	u32 i, j;
	const u8 *entry, *other;

	if (crc32(0, data, info->bytes) != info->table_crc)
		return -EBADMSG;
	/* Validate every used entry and overlap before printing any as accepted. */
	for (i = 0; i < info->count; i++) {
		entry = data + i * 128;
		if (!gp_entry_used(entry))
			continue;
		first = get_unaligned_le64(entry + 32);
		last = get_unaligned_le64(entry + 40);
		if (first < info->first || last > info->last || first > last)
			return -ERANGE;
		for (j = 0; j < i; j++) {
			other = data + j * 128;
			if (!gp_entry_used(other))
				continue;
			other_first = get_unaligned_le64(other + 32);
			other_last = get_unaligned_le64(other + 40);
			if (first <= other_last && last >= other_first)
				return -EINVAL;
		}
	}
	return 0;
}

#define GX_READ_LIMIT (128U * 1024 * 1024)
#define GX_FILE_LIMIT (64U * 1024 * 1024)

static struct {
	struct utp_transfer_req_desc *trd;
	struct utp_transfer_cmd_desc *ucd;
	u32 first, blocks, block_size, reads, bytes;
	ulong started;
	int error;
	bool active;
	struct udevice *dev;
	phys_addr_t dma;
	u32 old_lo, old_hi, old_rsr;
	bool changed;
} gx;

static ulong gx_read(struct udevice *dev, lbaint_t start,
		     lbaint_t count, void *buffer)
{
	u8 *dst = buffer;
	lbaint_t done = 0;
	u32 chunk;
	int ret;

	if (!gx.active || gx.error || !count || start >= gx.blocks ||
	    count > gx.blocks - start) {
		gx.error = gx.error ? gx.error : -ERANGE;
		return 0;
	}
	while (done < count) {
		chunk = min_t(lbaint_t, count - done,
			      GEMINI_DATA_SIZE / gx.block_size);
		if (get_timer(gx.started) > 120000 || gx.reads >= 100000 ||
		    gx.bytes > GX_READ_LIMIT - chunk * gx.block_size) {
			gx.error = -ETIMEDOUT;
			return done;
		}
		ret = gp_read_blocks(gx.trd, gx.ucd, 0,
				     gx.first + gx.blocks - 1, gx.block_size,
				     gx.first + start + done, chunk);
		gx.reads++;
		if (ret) {
			gx.error = ret;
			return done;
		}
		memcpy(dst + done * gx.block_size,
		       (u8 *)gx.trd + GEMINI_DATA_OFFSET,
		       chunk * gx.block_size);
		gx.bytes += chunk * gx.block_size;
		done += chunk;
	}
	return done;
}

static ulong gx_write(struct udevice *dev, lbaint_t start,
		      lbaint_t count, const void *buffer)
{
	gx.error = -EROFS;
	return (ulong)-EROFS;
}

static ulong gx_erase(struct udevice *dev, lbaint_t start, lbaint_t count)
{
	gx.error = -EROFS;
	return (ulong)-EROFS;
}

static const struct blk_ops gx_ops = {
	.read = gx_read,
	.write = gx_write,
	.erase = gx_erase,
};

U_BOOT_DRIVER(gemini_ufs_blk) = {
	.name = "gemini_ufs_blk",
	.id = UCLASS_BLK,
	.ops = &gx_ops,
};

static int gx_partition(struct utp_transfer_req_desc *trd,
			struct utp_transfer_cmd_desc *ucd)
{
	struct gp_gpt_info info;
	u8 *data = (u8 *)trd + GEMINI_DATA_OFFSET;
	u32 last, bs, found = 0;
	int ret, i, n;
	const u8 *entry;
	static const char name[] = "userdata";

	for (i = 0; i < 3; i++) {
		ret = gp_scsi(trd, ucd, 0, 0x25, 0, 0, 0);
		if (ret != -EAGAIN)
			break;
	}
	if (ret)
		return ret;
	last = get_unaligned_be32(data);
	bs = get_unaligned_be32(data + 4);
	if (last < 2 || last == 0xffffffff || (bs != 512 && bs != 4096))
		return -EINVAL;
	ret = gp_read_blocks(trd, ucd, 0, last, bs, 1, 1);
	if (!ret)
		ret = gp_gpt_header(data, bs, last, &info);
	if (!ret)
		ret = gp_read_blocks(trd, ucd, 0, last, bs, info.table,
				     DIV_ROUND_UP(info.bytes, bs));
	if (!ret)
		ret = gp_gpt_entries(0, data, &info);
	if (ret)
		return ret;
	for (i = 0; i < info.count; i++) {
		entry = data + i * 128;
		if (!gp_entry_used(entry))
			continue;
		for (n = 0; n < sizeof(name); n++)
			if (get_unaligned_le16(entry + 56 + n * 2) != name[n])
				break;
		if (n != sizeof(name))
			continue;
		if (found++)
			return -EEXIST;
		gx.first = get_unaligned_le64(entry + 32);
		gx.blocks = get_unaligned_le64(entry + 40) - gx.first + 1;
		gx.block_size = bs;
	}
	if (!found)
		return -ENOENT;
	printf("EXT4 PART lun=0 first=%u blocks=%u block_size=%u gpt_crc=PASS\n",
	       gx.first, gx.blocks, gx.block_size);
	return 0;
}

static int gx_super(const u8 *sb)
{
	u32 incompat = get_unaligned_le32(sb + 96);
	u32 ro = get_unaligned_le32(sb + 100);
	u16 state = get_unaligned_le16(sb + 58);

	printf("EXT4 SUPER state=%04x incompat=%08x ro_compat=%08x\n",
	       state, incompat, ro);
	/* Do not read a filesystem requiring journal replay, or unsupported features. */
	if (get_unaligned_le16(sb + 56) != 0xef53 ||
	    get_unaligned_le32(sb + 24) != 2 || state != 1 ||
	    (incompat & (EXT4_FEATURE_INCOMPAT_RECOVER |
			 EXT4_FEATURE_INCOMPAT_ENCRYPT)) ||
	    (incompat & ~(EXT4_FEATURE_INCOMPAT_SUPP |
			  EXT4_FEATURE_INCOMPAT_SUPP_LAZY_RO)) ||
	    (ro & EXT4_FEATURE_RO_COMPAT_BIGALLOC))
		return -EINVAL;
	puts("EXT4 clean=1 block_size=4096\n");
	return 0;
}


int gemini_ufs_close(bool restore)
{
	int ret = 0, err;

	if (gx.dev) {
		ret = device_remove(gx.dev, DM_REMOVE_NORMAL);
		if (!ret)
			ret = device_unbind(gx.dev);
		if (ret)
			return ret;
		gx.dev = NULL;
	}
	if (gx.changed) {
		gp_write(0, REG_UTP_TRANSFER_REQ_LIST_RUN_STOP);
		err = gp_wait(REG_UTP_TRANSFER_REQ_LIST_RUN_STOP, 1, 0, 1000);
		if (err || gp_read(REG_UTP_TRANSFER_REQ_DOOR_BELL) ||
		    gp_read(REG_UTP_TASK_REQ_DOOR_BELL)) {
			gemini_ufs_probe_blocked = true;
			/* Keep controller-owned DMA allocated until reset. */
			return err ? err : -EBUSY;
		}
		gp_write(gx.old_lo, REG_UTP_TRANSFER_REQ_LIST_BASE_L);
		gp_write(gx.old_hi, REG_UTP_TRANSFER_REQ_LIST_BASE_H);
		wmb();
		gp_write(restore ? gx.old_rsr : 0, REG_UTP_TRANSFER_REQ_LIST_RUN_STOP);
	}
	if (gx.dma)
		ret = lmb_free(gx.dma, GEMINI_DMA_SIZE, LMB_NOOVERWRITE);
	printf("UFS closed: reads=%u bytes=%u error=%d\n", gx.reads, gx.bytes, gx.error);
	memset(&gx, 0, sizeof(gx));
	return ret;
}

int gemini_ufs_open(struct blk_desc **desc)
{
	static const u16 attrs[] = {
		PA_PWRMODE, PA_TXGEAR, PA_RXGEAR, PA_ACTIVETXDATALANES,
		PA_ACTIVERXDATALANES, PA_HSSERIES,
	};
	u8 super[4096];
	u32 value;
	int ret, cleanup;

	if (gx.active || gx.dma || gemini_ufs_probe_blocked)
		return -EBUSY;
	ret = gp_guard();
	if (ret)
		return ret;
	for (int i = 0; i < ARRAY_SIZE(attrs); i++) {
		ret = gp_dme_get(attrs[i], &value);
		if (ret) {
			gemini_ufs_probe_blocked = true;
			return ret;
		}
	}
	gx.dma = 0x100000000ULL;
	ret = lmb_alloc_mem(LMB_MEM_ALLOC_MAX, 4096, &gx.dma,
			    GEMINI_DMA_SIZE, LMB_NOOVERWRITE);
	if (ret) {
		gx.dma = 0;
		return ret;
	}
	if (gx.dma < 0x80000000ULL || gx.dma + GEMINI_DMA_SIZE > 0x100000000ULL) {
		ret = -ERANGE;
		goto fail;
	}
	gx.trd = (void *)(ulong)gx.dma;
	gx.ucd = (void *)((ulong)gx.dma + GEMINI_UCD_OFFSET);
	gx.old_lo = gp_read(REG_UTP_TRANSFER_REQ_LIST_BASE_L);
	gx.old_hi = gp_read(REG_UTP_TRANSFER_REQ_LIST_BASE_H);
	gx.old_rsr = gp_read(REG_UTP_TRANSFER_REQ_LIST_RUN_STOP);
	if (gp_read(REG_UTP_TRANSFER_REQ_DOOR_BELL) ||
	    gp_read(REG_UTP_TASK_REQ_DOOR_BELL)) {
		ret = -EBUSY;
		goto fail;
	}
	gx.changed = true;
	gp_write(0, REG_UTP_TRANSFER_REQ_LIST_RUN_STOP);
	ret = gp_wait(REG_UTP_TRANSFER_REQ_LIST_RUN_STOP, 1, 0, 1000);
	if (ret)
		goto fail;
	gp_write(lower_32_bits(gx.dma), REG_UTP_TRANSFER_REQ_LIST_BASE_L);
	gp_write(0, REG_UTP_TRANSFER_REQ_LIST_BASE_H);
	wmb();
	gp_write(1, REG_UTP_TRANSFER_REQ_LIST_RUN_STOP);
	ret = gp_exchange(gx.trd, gx.ucd, false);
	if (!ret)
		ret = gp_exchange(gx.trd, gx.ucd, true);
	if (!ret)
		ret = gp_device_desc(gx.ucd);
	if (!ret)
		ret = gx_partition(gx.trd, gx.ucd);
	if (ret)
		goto fail;
	gx.active = true;
	gx.started = get_timer(0);
	if (gx_read(NULL, 0, sizeof(super) / gx.block_size, super) !=
	    sizeof(super) / gx.block_size) {
		ret = gx.error ? gx.error : -EIO;
		goto fail;
	}
	ret = gx_super(super + 1024);
	if (ret)
		goto fail;
	ret = blk_create_devicef(dm_root(), "gemini_ufs_blk", "gemini-ufs",
				 UCLASS_ROOT, -1, gx.block_size, gx.blocks, &gx.dev);
	if (!ret)
		ret = device_probe(gx.dev);
	if (ret || gx.error) {
		ret = gx.error ? gx.error : ret;
		goto fail;
	}
	*desc = dev_get_uclass_plat(gx.dev);
	return 0;
fail:
	cleanup = gemini_ufs_close(true);
	return cleanup ? cleanup : ret;
}

int gemini_ufs_error(void)
{
	return gx.error;
}

int gemini_ufs_quiesce(void)
{
	int ret = gp_guard();

	if (ret || gemini_ufs_probe_blocked)
		return ret ? ret : -EBUSY;
	ret = gemini_ufs_close(false);
	if (ret)
		return ret;
	gp_write(0, REG_UTP_TRANSFER_REQ_LIST_RUN_STOP);
	gp_write(0, REG_UTP_TASK_REQ_LIST_RUN_STOP);
	ret = gp_wait(REG_UTP_TRANSFER_REQ_LIST_RUN_STOP, 1, 0, 1000);
	if (!ret)
		ret = gp_wait(REG_UTP_TASK_REQ_LIST_RUN_STOP, 1, 0, 1000);
	if (ret || gp_read(REG_UTP_TRANSFER_REQ_DOOR_BELL) ||
	    gp_read(REG_UTP_TASK_REQ_DOOR_BELL))
		return ret ? ret : -EBUSY;
	return 0;
}
