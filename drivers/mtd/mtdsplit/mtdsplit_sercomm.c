/* Copyright (C) Tanaza S.P.A. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/byteorder/generic.h>

#include "mtdsplit.h"

/*
 * Sercomm image layout:
 *
 *      256 bytes       64 bytes
 * +----------------+---------------+---~~~~~~---+---~~~~~~---+
 * | Sercomm header | uImage header |   kernel   |   rootfs   |
 * +----------------+---------------+---~~~~~~---+---~~~~~~---+
 */

#define SERCOMM_MAGIC "Ser\0"
#define SERCOMM_MAGIC_LEN 4

/*
 * Header is 256 bytes length:
 *  - 56 bytes of struct fields
 *  - 200 bytes of pad
 */
#define SERCOMM_HEADER_LEN 256
#define SERCOMM_HEADER_PAD_LEN (SERCOMM_HEADER_LEN - sizeof(u32) * 14)

#define BOOTFLAG_PART "u-boot"
#define BOOTFLAG_OFFSET 0x3ff30
#define BOOTFLAG_FIRMWARE0 0xff
#define BOOTFLAG_FIRMWARE1 0x11

struct sercomm_header {
	u32 magic;			/* Sercomm magic */
	u32 pid_addr;			/* Length of final image */
	u32 hdr_crc32;			/* Header checksum */
	u8 img_num;			/* Always 0x02 */
	u8 cksum_bitmap;		/* Always 0xff */
	u16 reserved;			/* Always 0xffff */
	u32 kr_offset;			/* Kernel MTD offset */
	u32 kr_len;
	u32 kr_crc32;
	u32 pad_1;			/* Always 0x00000000 */
	u32 pad_2;			/* Always 0xffffffff */
	u32 pad_3;			/* Always 0xffffffff */
	u32 fs_offset;			/* Rootfs MTD offset */
	u32 fs_len;
	u32 fs_crc32;
	u32 pad_4;			/* Always 0x00000000 */
	u8 pad[SERCOMM_HEADER_PAD_LEN];	/* Unknown section */
};

static int get_bootflag(u8 *bf)
{
	struct mtd_info *mtd;
	size_t retlen;
	int r = 0;

	mtd = get_mtd_device_nm(BOOTFLAG_PART);
	if (IS_ERR(mtd))
		return -ENODEV;

	r = mtd_read(mtd, BOOTFLAG_OFFSET, sizeof(*bf), &retlen, bf);
	if (r != 0)
		goto done;

	if (retlen != sizeof(*bf))
		r = -EIO;
done:
	put_mtd_device(mtd);
	return r;
}

static int mtdsplit_sercomm_parse(struct mtd_info *mtd,
				  const struct mtd_partition **pparts,
				  struct mtd_part_parser_data *data)
{
	enum mtdsplit_part_type type;
	struct mtd_partition *parts;
	struct sercomm_header hdr;
	size_t retlen, tot_size, rootfs_offset;
	u8 bf;
	int r;

	r = get_bootflag(&bf);
	if (r != 0)
		return r;

	if (bf == BOOTFLAG_FIRMWARE0 && strcmp(mtd->name, "firmware0") != 0)
		return -ENODEV;

	if (bf == BOOTFLAG_FIRMWARE1 && strcmp(mtd->name, "firmware1") != 0)
		return -ENODEV;

	r = mtd_read(mtd, 0, SERCOMM_HEADER_LEN, &retlen, (void *)&hdr);
	if (r)
		return r;

	if (retlen != SERCOMM_HEADER_LEN)
		return -EIO;

	if (memcmp(&hdr.magic, SERCOMM_MAGIC, SERCOMM_MAGIC_LEN) != 0)
		return -EINVAL;

	tot_size = le32_to_cpu(hdr.kr_len) + le32_to_cpu(hdr.fs_len) +
		   SERCOMM_HEADER_LEN;
	if (tot_size > mtd->size)
		return -EINVAL;

	/* Find the rootfs after the uImage */
	r = mtd_find_rootfs_from(mtd,
				 le32_to_cpu(hdr.kr_len) + SERCOMM_HEADER_LEN,
				 mtd->size, &rootfs_offset, &type);
	if (r)
		return -EINVAL;

	parts = kzalloc(2 * sizeof(*parts), GFP_KERNEL);
	if (!parts)
		return -ENOMEM;

	/*
	 * The kernel partition includes the
	 * Sercomm header and the uImage header
	 */
	parts[0].name = KERNEL_PART_NAME;
	parts[0].offset = 0;
	parts[0].size = rootfs_offset;

	parts[1].name = ROOTFS_PART_NAME;
	parts[1].offset = rootfs_offset;
	parts[1].size = le32_to_cpu(hdr.fs_len);

	*pparts = parts;
	return 2;
}

static const struct of_device_id mtdsplit_sercomm_of_match_table[] = {
	{ .compatible = "sercomm" },
	{},
};

static struct mtd_part_parser sercomm_parser = {
	.owner = THIS_MODULE,
	.name = "sercomm-fw",
	.of_match_table = mtdsplit_sercomm_of_match_table,
	.parse_fn = mtdsplit_sercomm_parse,
	.type = MTD_PARSER_TYPE_FIRMWARE,
};
module_mtd_part_parser(sercomm_parser);
