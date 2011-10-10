/*
 * Copyright (c) 2011 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define _GNU_SOURCE
#include <confuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>

#include "genimage.h"

struct hdimage {
	cfg_bool_t partition_table;
};

struct partition_entry {
	unsigned char boot;

	unsigned char first_chs[3];

	unsigned char partition_type;

	unsigned char last_chs[3];

	uint32_t relative_sectors;
	uint32_t total_sectors;
};

static void hdimage_setup_chs(unsigned int lba, unsigned char *chs)
{
	const unsigned int hpc = 255;
	const unsigned int spt = 63;
	unsigned int s, c;

	chs[0] = (lba/spt)%hpc;
	c = (lba/(spt * hpc));
	s = (lba > 0) ?(lba%spt + 1) : 0;
	chs[1] = ((c & 0x300) >> 2) | (s & 0xff);
	chs[2] = (c & 0xff);
}

static int hdimage_setup_mbr(struct image *image, char *part_table)
{
	struct partition *part;
	int i = 0;

	image_log(image, 1, "writing MBR\n");
	list_for_each_entry(part, &image->partitions, list) {
		struct partition_entry *entry;

		if (!part->in_partition_table)
			continue;

		if (i > 3) {
			image_error(image, "cannot handle more than 4 partitions");
			return -EINVAL;
		}
		entry = (struct partition_entry *)(part_table + i *
				sizeof(struct partition_entry));

		entry->partition_type = part->partition_type;
		entry->relative_sectors = part->offset/512;
		entry->total_sectors = part->size/512;
		hdimage_setup_chs(entry->relative_sectors, entry->first_chs);
		hdimage_setup_chs(entry->relative_sectors +
				entry->total_sectors - 1, entry->last_chs);

		i++;
	}
	part_table += 4 * sizeof(struct partition_entry);
	part_table[0] = 0x55;
	part_table[1] = 0xaa;
	return 0;
}

static int hdimage_generate(struct image *image)
{
	struct partition *part;
	struct hdimage *hd = image->handler_priv;
	enum pad_mode mode = MODE_OVERWRITE;
	const char *outfile = imageoutfile(image);
	int ret;

	list_for_each_entry(part, &image->partitions, list) {
		struct image *child;
		const char *infile;

		child = image_get(part->image);
		if (!child) {
			image_error(image, "could not find %s\n", part->image);
			return -EINVAL;
		}
		infile = imageoutfile(child);

		ret = pad_file(NULL, outfile, part->offset, 0x0, mode);
		if (ret) {
			image_error(image, "failed to pad image to size %lld\n",
					part->offset);
			return ret;
		}

		ret = pad_file(infile, outfile, part->size, 0x0, MODE_APPEND);

		if (ret) {
			image_error(image, "failed to write image partition '%s'\n",
					part->name);
			return ret;
		}
		mode = MODE_APPEND;
	}

	if (hd->partition_table) {
		char part_table[4*sizeof(struct partition_entry)+2];

		memset(part_table, 0, sizeof(part_table));
		ret = hdimage_setup_mbr(image, part_table);
		if (ret)
			return ret;

		ret = insert_data(part_table, outfile, sizeof(part_table), 446);
		if (ret) {
			image_error(image, "failed to write MBR\n");
			return ret;
		}
		mode = MODE_APPEND;
	}

	return 0;
}

static int hdimage_setup(struct image *image, cfg_t *cfg)
{
	struct partition *part;
	unsigned long long now = 0;
	unsigned long long align = cfg_getint_suffix(cfg, "align");
	struct hdimage *hd = xzalloc(sizeof(*hd));

	hd->partition_table = cfg_getbool(cfg, "partition-table");

	if ((align % 512) || (align == 0)) {
		image_error(image, "partition alignment (%lld) must be a"
				"multiple of 1 sector (512 bytes)\n", align);
		return -EINVAL;
	}
	list_for_each_entry(part, &image->partitions, list) {
		if (part->size % 512) {
			image_error(image, "part %s size (%lld) must be a"
					"multiple of 1 sector (512 bytes)\n",
					part->name, part->size);
			return -EINVAL;
		}
		if (part->in_partition_table && (part->offset % align)) {
			image_error(image, "part %s offset (%lld) must be a"
					"multiple of %lld bytes\n",
					part->name, part->offset, align);
			return -EINVAL;
		}
		if (part->offset || !part->in_partition_table) {
			if (now > part->offset) {
				image_error(image, "part %s overlaps with previous partition\n",
						part->name);
				return -EINVAL;
			}
		} else {
			if (!now && hd->partition_table)
				now = 512;
			part->offset = ((now -1)/align + 1) * align;
		}
		now = part->offset + part->size;
	}

	if (now > image->size) {
		image_error(image, "partitions exceed device size\n");
		return -EINVAL;
	}

	image->handler_priv = hd;

	return 0;
}

cfg_opt_t hdimage_opts[] = {
	CFG_STR("align", "512", CFGF_NONE),
	CFG_BOOL("partition-table", cfg_true, CFGF_NONE),
	CFG_END()
};

struct image_handler hdimage_handler = {
	.type = "hdimage",
	.generate = hdimage_generate,
	.setup = hdimage_setup,
	.opts = hdimage_opts,
};

