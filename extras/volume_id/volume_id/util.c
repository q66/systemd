/*
 * volume_id - reads filesystem label and uuid
 *
 * Copyright (C) 2005 Kay Sievers <kay.sievers@vrfy.org>
 *
 *	This library is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU Lesser General Public
 *	License as published by the Free Software Foundation; either
 *	version 2.1 of the License, or (at your option) any later version.
 *
 *	This library is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *	Lesser General Public License for more details.
 *
 *	You should have received a copy of the GNU Lesser General Public
 *	License along with this library; if not, write to the Free Software
 *	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <asm/types.h>

#include "volume_id.h"
#include "logging.h"
#include "util.h"

static char *usage_to_string(enum volume_id_usage usage_id)
{
	switch (usage_id) {
	case VOLUME_ID_FILESYSTEM:
		return "filesystem";
	case VOLUME_ID_PARTITIONTABLE:
		return "partitiontable";
	case VOLUME_ID_OTHER:
		return "other";
	case VOLUME_ID_RAID:
		return "raid";
	case VOLUME_ID_DISKLABEL:
		return "disklabel";
	case VOLUME_ID_CRYPTO:
		return "crypto";
	case VOLUME_ID_UNPROBED:
		return "unprobed";
	case VOLUME_ID_UNUSED:
		return "unused";
	}
	return NULL;
}

void volume_id_set_usage_part(struct volume_id_partition *part, enum volume_id_usage usage_id)
{
	part->usage_id = usage_id;
	part->usage = usage_to_string(usage_id);
}

void volume_id_set_usage(struct volume_id *id, enum volume_id_usage usage_id)
{
	id->usage_id = usage_id;
	id->usage = usage_to_string(usage_id);
}

void volume_id_set_label_raw(struct volume_id *id, const __u8 *buf, unsigned int count)
{
	memcpy(id->label_raw, buf, count);
	id->label_raw_len = count;
}

void volume_id_set_label_string(struct volume_id *id, const __u8 *buf, unsigned int count)
{
	unsigned int i;

	memcpy(id->label, buf, count);

	/* remove trailing whitespace */
	i = strnlen(id->label, count);
	while (i--) {
		if (!isspace(id->label[i]))
			break;
	}
	id->label[i+1] = '\0';
}

void volume_id_set_label_unicode16(struct volume_id *id, const __u8 *buf, enum endian endianess, unsigned int count)
{
	unsigned int i, j;
	__u16 c;

	j = 0;
	for (i = 0; i + 2 <= count; i += 2) {
		if (endianess == LE)
			c = (buf[i+1] << 8) | buf[i];
		else
			c = (buf[i] << 8) | buf[i+1];
		if (c == 0) {
			id->label[j] = '\0';
			break;
		} else if (c < 0x80) {
			id->label[j++] = (__u8) c;
		} else if (c < 0x800) {
			id->label[j++] = (__u8) (0xc0 | (c >> 6));
			id->label[j++] = (__u8) (0x80 | (c & 0x3f));
		} else {
			id->label[j++] = (__u8) (0xe0 | (c >> 12));
			id->label[j++] = (__u8) (0x80 | ((c >> 6) & 0x3f));
			id->label[j++] = (__u8) (0x80 | (c & 0x3f));
		}
	}
}

void volume_id_set_uuid(struct volume_id *id, const __u8 *buf, enum uuid_format format)
{
	unsigned int i;
	unsigned int count = 0;

	switch(format) {
	case UUID_DOS:
		count = 4;
		break;
	case UUID_NTFS:
	case UUID_HFS:
		count = 8;
		break;
	case UUID_DCE:
		count = 16;
		break;
	case UUID_DCE_STRING:
		count = 36;
		break;
	}
	memcpy(id->uuid_raw, buf, count);
	id->uuid_raw_len = count;

	/* if set, create string in the same format, the native platform uses */
	for (i = 0; i < count; i++)
		if (buf[i] != 0)
			goto set;
	return;

set:
	switch(format) {
	case UUID_DOS:
		sprintf(id->uuid, "%02X%02X-%02X%02X",
			buf[3], buf[2], buf[1], buf[0]);
		break;
	case UUID_NTFS:
		sprintf(id->uuid,"%02X%02X%02X%02X%02X%02X%02X%02X",
			buf[7], buf[6], buf[5], buf[4],
			buf[3], buf[2], buf[1], buf[0]);
		break;
	case UUID_HFS:
		sprintf(id->uuid,"%02X%02X%02X%02X%02X%02X%02X%02X",
			buf[0], buf[1], buf[2], buf[3],
			buf[4], buf[5], buf[6], buf[7]);
		break;
	case UUID_DCE:
		sprintf(id->uuid,
			"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			buf[0], buf[1], buf[2], buf[3],
			buf[4], buf[5],
			buf[6], buf[7],
			buf[8], buf[9],
			buf[10], buf[11], buf[12], buf[13], buf[14],buf[15]);
		break;
	case UUID_DCE_STRING:
		memcpy(id->uuid, buf, count);
		id->uuid[count] = '\0';
		break;
	}
}

__u8 *volume_id_get_buffer(struct volume_id *id, __u64 off, unsigned int len)
{
	unsigned int buf_len;

	dbg("get buffer off 0x%llx(%llu), len 0x%x", (unsigned long long) off, (unsigned long long) off, len);
	/* check if requested area fits in superblock buffer */
	if (off + len <= SB_BUFFER_SIZE) {
		if (id->sbbuf == NULL) {
			id->sbbuf = malloc(SB_BUFFER_SIZE);
			if (id->sbbuf == NULL)
				return NULL;
		}

		/* check if we need to read */
		if ((off + len) > id->sbbuf_len) {
			dbg("read sbbuf len:0x%llx", (unsigned long long) (off + len));
			lseek(id->fd, 0, SEEK_SET);
			buf_len = read(id->fd, id->sbbuf, off + len);
			dbg("got 0x%x (%i) bytes", buf_len, buf_len);
			id->sbbuf_len = buf_len;
			if (buf_len < off + len)
				return NULL;
		}

		return &(id->sbbuf[off]);
	} else {
		if (len > SEEK_BUFFER_SIZE) {
			dbg("seek buffer too small %d", SEEK_BUFFER_SIZE);
			return NULL;
		}

		/* get seek buffer */
		if (id->seekbuf == NULL) {
			id->seekbuf = malloc(SEEK_BUFFER_SIZE);
			if (id->seekbuf == NULL)
				return NULL;
		}

		/* check if we need to read */
		if ((off < id->seekbuf_off) || ((off + len) > (id->seekbuf_off + id->seekbuf_len))) {
			dbg("read seekbuf off:0x%llx len:0x%x", (unsigned long long) off, len);
			if (lseek(id->fd, off, SEEK_SET) == -1)
				return NULL;
			buf_len = read(id->fd, id->seekbuf, len);
			dbg("got 0x%x (%i) bytes", buf_len, buf_len);
			id->seekbuf_off = off;
			id->seekbuf_len = buf_len;
			if (buf_len < len) {
				dbg("requested 0x%x bytes, got only 0x%x bytes", len, buf_len);
				return NULL;
			}
		}

		return &(id->seekbuf[off - id->seekbuf_off]);
	}
}

void volume_id_free_buffer(struct volume_id *id)
{
	if (id->sbbuf != NULL) {
		free(id->sbbuf);
		id->sbbuf = NULL;
		id->sbbuf_len = 0;
	}
	if (id->seekbuf != NULL) {
		free(id->seekbuf);
		id->seekbuf = NULL;
		id->seekbuf_len = 0;
	}
}
