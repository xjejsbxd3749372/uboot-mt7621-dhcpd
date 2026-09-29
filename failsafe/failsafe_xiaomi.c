/* SPDX-License-Identifier:	GPL-2.0 */
/*
 * Copyright (C) 2026 Yuzhii0718
 *
 * All rights reserved.
 *
 * This file is part of the project uboot-mt7621-dhcpd
 * You may not use, copy, modify or distribute this file except with the license agreement.
 *
 * Extended failsafe Web UI features for Xiaomi MT7621 routers:
 *   - Xiaomi stock (HDR1) firmware restore
 *   - CPU frequency (overclock) control, applied on the next OS boot
 *   - Config / Bdata / Factory-MAC parameter editor with CRC32 verification
 *
 * The HTTP contracts implemented here match the recovery UI shipped with the
 * Xiaomi R2100/RM2100 (AC2100) builds: /cpufreq, /params and the xiaomi_stock
 * upload channel.
 */

#include <common.h>
#include <command.h>
#include <errno.h>
#include <image.h>
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <malloc.h>
#include <u-boot/crc.h>
#include <asm/global_data.h>
#ifdef CONFIG_MACH_MT7621
#include <asm/io.h>
#include <asm/addrspace.h>
#include <mach/mt7621_regs.h>
#include "clocks.h"
#endif
#include <net/httpd.h>

#ifdef CONFIG_MTD
#include <linux/mtd/mtd.h>
#include <spi_flash.h>
#endif
#ifdef CONFIG_CMD_MTDPARTS
#include <jffs2/load_kernel.h>
#endif

#include "flash_helper.h"

#include "failsafe_internal.h"

DECLARE_GLOBAL_DATA_PTR;

#define FS_COLOR_PROMPT	"\x1b[0;33m"
#define FS_COLOR_ERROR	"\x1b[93;41m"
#define FS_COLOR_NORMAL	"\x1b[0m"

/*
 * Environment variables carrying the overclock setting from the failsafe Web
 * UI to the next operating system boot.  The value is consumed by
 * xiaomi_apply_saved_cpu_freq() just before control is handed to the kernel.
 */
#define ENV_OC_ENABLED		"failsafe_oc_enabled"
#define ENV_OC_MHZ		"failsafe_oc_mhz"

/* Frequency bounds accepted by the Web UI and by the apply step. */
#define OC_MIN_MHZ		400
#define OC_MAX_MHZ		1000
#define OC_STEP_MHZ		5

/*
 * Offsets of the four interface MAC fields inside Xiaomi's Factory partition.
 * Byte offsets into the 256 KiB factory area; shared by every MT7621 Xiaomi
 * router using the same nand_128m layout.
 */
#define XIAOMI_MAC_RF1_OFF	0x0000
#define XIAOMI_MAC_RF2_OFF	0x8000
#define XIAOMI_MAC_LAN_OFF	0xe000
#define XIAOMI_MAC_WAN_OFF	0xe006

/*
 * Stock parameter record sizes, in bytes.  The *offsets* are never
 * hardcoded: on an MT7621 Xiaomi router the U-Boot environment itself lives
 * at CONFIG_ENV_OFFSET (0x80000 on the NAND template), which is exactly
 * where Xiaomi's Config record would live in a genuine stock flash.  Every
 * region below is therefore located at runtime through get_mtd_part_info()
 * by partition name, and the whole parameter editor refuses to run unless the
 * runtime table matches the stock layout.  This is what keeps the feature
 * from writing over the U-Boot environment.
 */

/* Config is the environment-sized record; Bdata is the larger identity blob. */
#define XIAOMI_CONFIG_SIZE	0x1000
#define XIAOMI_BDATA_SIZE	0x4000

/*
 * Stock layout this module targets, expressed as the partition names and
 * sizes that must be present.  Compared against the live table before any
 * write; a mismatch disables the editor instead of risking the device.
 */
#define XIAOMI_FACTORY_NAME	"factory"
#define XIAOMI_FACTORY_SIZE	0x40000

extern int write_firmware_failsafe(size_t data_addr, uint32_t data_size);
extern int write_uboot_failsafe(size_t data_addr, uint32_t data_size);
extern int write_factory_failsafe(size_t data_addr, uint32_t data_size);
extern int get_mtd_part_info(const char *partname, uint64_t *off, uint64_t *size);

struct xiaomi_regions;

static int xiaomi_flash_erase_write_verify(void *flash, uint64_t off,
					   uint64_t len, const void *src);
static char *xiaomi_build_params_json(void *flash,
				      const struct xiaomi_regions *reg);
static int xiaomi_write_partition(void *flash, const char *name,
				  uint64_t part_off, uint64_t part_size,
				  uint32_t erase_size, const void *src, uint32_t len);

/* ------------------------------------------------------------------ */
/* Runtime partition lookup                                            */
/* ------------------------------------------------------------------ */

/*
 * Locate the stock parameter records by partition name.
 *
 * A board's MTD table decides where everything physically lives, and the
 * U-Boot environment shares the first kilobytes of the old stock Config
 * area.  Nothing is written until the live table has been checked against
 * the expected stock geometry, so a custom mtdparts can never make this code
 * erase the environment or the bootloader.
 */
struct xiaomi_regions {
	uint64_t config_off;
	uint64_t config_size;
	uint64_t bdata_off;
	uint64_t bdata_size;
	uint64_t factory_off;
	uint64_t factory_size;
};

static const char * const xiaomi_param_part_names[] = {
	"config", "bdata", "factory"
};

static int xiaomi_resolve_regions(struct xiaomi_regions *r)
{
	uint64_t config_off = 0, config_size = 0;
	uint64_t bdata_off = 0, bdata_size = 0;
	uint64_t factory_off = 0, factory_size = 0;
	uint64_t tmp;

	/*
	 * The stock record names are used when the board provides them.  A
	 * stock-flashed Xiaomi router (the only case where the editor makes
	 * sense) has 'config' and 'bdata' alongside 'factory'.
	 */
	if (get_mtd_part_info("config", &config_off, &config_size) ||
	    get_mtd_part_info("bdata", &bdata_off, &bdata_size)) {
		printf("\n" FS_COLOR_ERROR "*** MTD partitions 'config' and "
		       "'bdata' were not found ***" FS_COLOR_NORMAL "\n");
		return -EINVAL;
	}

	if (get_mtd_part_info(XIAOMI_FACTORY_NAME, &factory_off, &factory_size))
		return -EINVAL;

	/* Refuse anything that does not look like the stock geometry */
	if (config_size < XIAOMI_CONFIG_SIZE ||
	    bdata_size < XIAOMI_BDATA_SIZE ||
	    factory_size < XIAOMI_FACTORY_SIZE) {
		printf("\n" FS_COLOR_ERROR "*** MTD layout is too small for the "
		       "Xiaomi stock parameter records ***" FS_COLOR_NORMAL "\n");
		return -EINVAL;
	}

	/* The U-Boot environment must not overlap the Config record */
	{
		uint64_t env_off = 0, env_size = 0;

		if (!get_mtd_part_info("u-boot-env", &env_off, &env_size) &&
		    env_size) {
			if (env_off < config_off + XIAOMI_CONFIG_SIZE &&
			    config_off < env_off + env_size) {
				printf("\n" FS_COLOR_ERROR "*** MTD partition "
				       "'config' overlaps 'u-boot-env'; refusing "
				       "to touch the environment ***"
				       FS_COLOR_NORMAL "\n");
				return -EINVAL;
			}
		}
	}

	/* Each record must begin on an erase-block boundary */
	tmp = config_off;
	if (do_div(tmp, XIAOMI_CONFIG_SIZE)) {
		printf("\n" FS_COLOR_ERROR "*** MTD partition 'config' is not "
		       "aligned to the Config record size ***" FS_COLOR_NORMAL "\n");
		return -EINVAL;
	}

	tmp = bdata_off;
	if (do_div(tmp, XIAOMI_BDATA_SIZE)) {
		printf("\n" FS_COLOR_ERROR "*** MTD partition 'bdata' is not "
		       "aligned to the Bdata record size ***" FS_COLOR_NORMAL "\n");
		return -EINVAL;
	}

	r->config_off = config_off;
	r->config_size = XIAOMI_CONFIG_SIZE;
	r->bdata_off = bdata_off;
	r->bdata_size = XIAOMI_BDATA_SIZE;
	r->factory_off = factory_off;
	r->factory_size = XIAOMI_FACTORY_SIZE;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int json_escape_x(char *dst, size_t dst_sz, const char *src)
{
	size_t di = 0;
	const unsigned char *s = (const unsigned char *)src;

	if (!dst || !dst_sz)
		return 0;

	dst[0] = '\0';
	if (!src)
		return 0;

	while (*s && di + 2 < dst_sz) {
		unsigned char c = *s++;

		if (c == '"' || c == '\\') {
			if (di + 2 >= dst_sz)
				break;
			dst[di++] = '\\';
			dst[di++] = (char)c;
			continue;
		}

		if (c < 0x20) {
			dst[di++] = ' ';
			continue;
		}

		dst[di++] = (char)c;
	}

	dst[di] = '\0';
	return di;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Parse "xx:xx:xx:xx:xx:xx" into 6 bytes.  Rejects multicast (odd first
 * octet) and all-zero / all-ones addresses, mirroring the Web UI's own
 * validation so the two never disagree.
 */
static int parse_mac(const char *s, u8 out[6])
{
	int i, hi, lo;

	if (!s)
		return -EINVAL;

	/* Reject anything that is not exactly "xx:xx:xx:xx:xx:xx" up front so
	 * the loop below can never read past the terminator. */
	if (strlen(s) != 17)
		return -EINVAL;

	for (i = 0; i < 6; i++) {
		hi = hexval(s[0]);
		lo = hexval(s[1]);
		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[i] = (u8)((hi << 4) | lo);
		s += 2;
		if (i < 5) {
			if (*s != ':')
				return -EINVAL;
			s++;
		}
	}

	if (out[0] & 0x01)
		return -EINVAL;			/* multicast / broadcast */

	if (!(out[0] | out[1] | out[2] | out[3] | out[4] | out[5]))
		return -EINVAL;			/* all zero */

	if (!(out[0] & out[1] & out[2] & out[3] & out[4] & out[5]))
		return -EINVAL;			/* all ones */

	return 0;
}

static void format_mac(char *dst, size_t dst_sz, const u8 mac[6])
{
	snprintf(dst, dst_sz, "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/* Flash access helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * Erase, program and read back one region, byte-comparing what was written.
 * Comparing immediately (rather than only reporting success) is what makes
 * the "written and read-back verified" claim in the UI truthful.
 */
static int xiaomi_flash_erase_write_verify(void *flash, uint64_t off,
					   uint64_t len, const void *src)
{
	const u8 *p = (const u8 *)src;
	uint8_t *verify;
	uint64_t done = 0;
	int ret;

	ret = mtk_board_flash_erase(flash, off, len);
	if (ret)
		return ret;

	verify = malloc(len);
	if (!verify)
		return -ENOMEM;

	while (done < len) {
		uint64_t chunk = len - done;

		if (chunk > SZ_256K)
			chunk = SZ_256K;

		ret = mtk_board_flash_write(flash, off + done,
					     (size_t)chunk, p + done);
		if (ret) {
			free(verify);
			return ret;
		}

		ret = mtk_board_flash_read(flash, off + done,
					   (size_t)chunk, verify);
		if (ret) {
			free(verify);
			return ret;
		}

		if (memcmp(verify, p + done, (size_t)chunk)) {
			free(verify);
			return -EIO;
		}

		done += chunk;
	}

	free(verify);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Config / Bdata parameter records                                   */
/* ------------------------------------------------------------------ */

/*
 * Xiaomi stores the stock boot environment as a plain "key=value" blob
 * terminated by a NUL, with a 4-byte CRC32 of the preceding bytes in the last
 * word of the record.  Returns the text length, negated when the CRC does not
 * match, so the caller can both render the content and refuse to write back
 * a record whose CRC is already broken.
 */
static int param_record_parse(const u8 *data, size_t size, char *out,
			      size_t out_sz)
{
	const char *start = (const char *)data;
	const char *end = start + size;
	const char *p = start;
	size_t text_len, copy;
	u32 stored_crc, calc_crc;

	if (!out || out_sz < 2 || size < 5)
		return 0;

	out[0] = '\0';

	while (p < end && *p)
		p++;

	if (p >= end)
		return 0;			/* unterminated: not a valid record */

	text_len = p - start;

	memcpy(&stored_crc, data + size - 4, 4);
	calc_crc = crc32(0, data, size - 4);

	copy = text_len;
	if (copy >= out_sz)
		copy = out_sz - 1;
	memcpy(out, start, copy);
	out[copy] = '\0';

	return (calc_crc == stored_crc) ? (int)text_len : -(int)text_len;
}

/*
 * Rebuild a parameter record: text blob, NUL terminator, 0xFF padding up to
 * (size - 4), then the CRC32 over everything preceding the CRC field.
 */
static int param_record_build(const char *text, size_t text_len, u8 *out,
			      size_t size)
{
	u32 crc;

	if (text_len + 5 > size)
		return -EINVAL;

	memset(out, 0xFF, size);
	memcpy(out, text, text_len);
	out[text_len] = '\0';

	crc = crc32(0, out, size - 4);
	memcpy(out + size - 4, &crc, 4);

	return 0;
}

/*
 * Validate a submitted parameter table: at least one entry, every line must
 * carry a non-empty key containing neither '=' nor a newline, keys must be
 * unique, and the total must fit the target record.
 */
#define PARAM_MAX_KEYS	64

static int param_text_validate(const char *text, size_t record_size,
			       size_t *text_len_out)
{
	const char *p = text;
	size_t total = 0;
	unsigned count = 0;
	char seen[PARAM_MAX_KEYS][160];
	unsigned i;

	if (!text || !text[0])
		return -EINVAL;

	while (*p) {
		const char *eol = strchr(p, '\n');
		const char *eq;
		const char *q;
		size_t klen;
		bool dup = false;

		if (!eol)
			eol = p + strlen(p);

		/* Blank lines are skipped, matching the Web UI serializer */
		if (eol != p) {
			eq = memchr(p, '=', eol - p);
			if (!eq || eq == p)
				return -EINVAL;

			klen = (size_t)(eq - p);
			if (klen >= sizeof(seen[0]))
				return -EINVAL;

			/* Key must be free of control characters */
			for (q = p; q < eq; q++) {
				if (*q < 0x20 || *q == 0x7f)
					return -EINVAL;
			}

			/* Value must not contain CR (LF already consumed) */
			if (memchr(eq + 1, '\r', eol - eq - 1))
				return -EINVAL;

			if (count >= PARAM_MAX_KEYS)
				return -EINVAL;

			memcpy(seen[count], p, klen);
			seen[count][klen] = '\0';

			/* Keys must be unique */
			for (i = 0; i < count; i++) {
				if (!strcmp(seen[i], seen[count])) {
					dup = true;
					break;
				}
			}
			if (dup)
				return -EINVAL;

			count++;
			total += (eol - p) + 1;
		}

		if (!*eol)
			break;
		p = eol + 1;
	}

	if (!count)
		return -EINVAL;

	if (total + 5 > record_size)
		return -EINVAL;

	*text_len_out = total;
	return 0;
}

/* ------------------------------------------------------------------ */
/* MAC layout validation                                              */
/* ------------------------------------------------------------------ */

/*
 * Xiaomi's four interface MACs are consecutive in WAN -> LAN -> RF1 -> RF2
 * order.  Accept only that exact layout: one 40-bit prefix with the last
 * octet increasing by one per interface, and no wrap past 0xFD.  Anything
 * else means the factory blob does not match a known layout, in which case a
 * write could silently relocate an interface's identity.
 */
static int macs_layout_ok(const u8 wan[6], const u8 lan[6],
			  const u8 rf1[6], const u8 rf2[6])
{
	if (memcmp(wan, lan, 5) || memcmp(wan, rf1, 5) || memcmp(wan, rf2, 5))
		return 0;

	if (wan[5] > 0xfc)
		return 0;

	if ((int)lan[5] != (int)wan[5] + 1)
		return 0;
	if ((int)rf1[5] != (int)wan[5] + 2)
		return 0;
	if ((int)rf2[5] != (int)wan[5] + 3)
		return 0;

	return 1;
}

/* ------------------------------------------------------------------ */
/* Percent-decoding                                                    */
/* ------------------------------------------------------------------ */

/*
 * The recovery UI submits the parameter table as
 * encodeURIComponent(key) + "=" + encodeURIComponent(value) joined by
 * newlines.  U-Boot's httpd hands form values over verbatim, so decoding
 * happens here.  Decodes in place (output never exceeds input length) and
 * rejects truncated escapes so malformed input cannot smuggle in bytes.
 */
static int urldecode_inplace(char *s)
{
	char *r = s, *w = s;

	while (*r) {
		if (*r == '%') {
			int hi, lo;

			if (!r[1] || !r[2])
				return -EINVAL;

			hi = hexval(r[1]);
			lo = hexval(r[2]);
			if (hi < 0 || lo < 0)
				return -EINVAL;

			*w++ = (char)((hi << 4) | lo);
			r += 3;
			continue;
		}

		if (*r == '+') {
			*w++ = ' ';
			r++;
			continue;
		}

		*w++ = *r++;
	}

	*w = '\0';
	return 0;
}

/* ------------------------------------------------------------------ */
/* /cpufreq  -  CPU frequency control                                 */
/* ------------------------------------------------------------------ */

static bool oc_is_valid_mhz(unsigned mhz)
{
	if (mhz < OC_MIN_MHZ || mhz > OC_MAX_MHZ)
		return false;
	if (mhz % OC_STEP_MHZ)
		return false;
	return true;
}

static unsigned oc_safe_mhz(void)
{
#if defined(CONFIG_MT7621_CPU_FREQ_LEGACY) && CONFIG_MT7621_CPU_FREQ_LEGACY
	return CONFIG_MT7621_CPU_FREQ_LEGACY;
#elif defined(CONFIG_MT7621_CPU_FREQ) && CONFIG_MT7621_CPU_FREQ
	return CONFIG_MT7621_CPU_FREQ;
#else
	return 880;
#endif
}

static unsigned oc_current_mhz(void)
{
	unsigned long hz = get_cpu_freq(0);
	unsigned mhz = hz ? (unsigned)(hz / 1000000U) : 0;

	return mhz ? mhz : oc_safe_mhz();
}

static void oc_build_json(char *buf, size_t buf_sz, bool ok, int error,
			  bool enabled, unsigned configured_mhz,
			  unsigned current_mhz)
{
	snprintf(buf, buf_sz,
		 "{\"ok\":%s,\"error\":%d,\"enabled\":%s,\"safe_mhz\":%u,"
		 "\"current_mhz\":%u,\"configured_mhz\":%u,"
		 "\"min_mhz\":%u,\"max_mhz\":%u,\"step_mhz\":%u}",
		 ok ? "true" : "false", error, enabled ? "true" : "false",
		 oc_safe_mhz(), current_mhz,
		 configured_mhz ? configured_mhz : oc_safe_mhz(),
		 (unsigned)OC_MIN_MHZ, (unsigned)OC_MAX_MHZ,
		 (unsigned)OC_STEP_MHZ);
}

static void oc_respond(struct httpd_response *response, const char *json)
{
	response->status = HTTP_RESP_STD;
	response->data = json;
	response->size = strlen(json);
	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "application/json";
}

/*
 * Report or change the CPU frequency used for the next operating system
 * boot.  Failsafe itself always runs at the compile-time safe frequency, so
 * a bad setting can still be cleared from this page.
 */
void cpufreq_handler(enum httpd_uri_handler_status status,
		     struct httpd_request *request,
		     struct httpd_response *response)
{
	char json[512];
	const char *saved_enabled, *saved_mhz;
	bool enabled = false;
	unsigned configured = 0, current;
	struct httpd_form_value *action, *mhz_val;
	int error = 0;

	if (status != HTTP_CB_NEW)
		return;

	saved_enabled = env_get(ENV_OC_ENABLED);
	saved_mhz = env_get(ENV_OC_MHZ);

	if (saved_enabled && !strcmp(saved_enabled, "1")) {
		unsigned v = saved_mhz ? simple_strtoul(saved_mhz, NULL, 10) : 0;

		if (oc_is_valid_mhz(v)) {
			enabled = true;
			configured = v;
		} else {
			printf("\n" FS_COLOR_ERROR "*** Stored CPU frequency "
			       "is out of range; using safe frequency ***"
			       FS_COLOR_NORMAL "\n");
		}
	}

	current = oc_current_mhz();

	if (request->method != HTTP_POST) {
		oc_build_json(json, sizeof(json), true, 0, enabled, configured,
			      current);
		oc_respond(response, json);
		return;
	}

	action = httpd_request_find_value(request, "action");
	if (!action) {
		oc_build_json(json, sizeof(json), false, -EINVAL, enabled,
			      configured, current);
		oc_respond(response, json);
		return;
	}

	if (!strcmp(action->data, "disable")) {
		env_set(ENV_OC_ENABLED, "0");
		env_set(ENV_OC_MHZ, "0");
		printf("\n" FS_COLOR_PROMPT "*** CPU overclock disabled; safe "
		       "frequency will be used ***" FS_COLOR_NORMAL "\n");
		enabled = false;
		configured = 0;
	} else if (!strcmp(action->data, "set")) {
		unsigned mhz;
		char mhz_str[16];

		mhz_val = httpd_request_find_value(request, "mhz");
		if (!mhz_val) {
			oc_build_json(json, sizeof(json), false, -EINVAL,
				      enabled, configured, current);
			oc_respond(response, json);
			return;
		}

		mhz = simple_strtoul(mhz_val->data, NULL, 10);

		if (!oc_is_valid_mhz(mhz)) {
			printf("\n" FS_COLOR_ERROR "*** Invalid CPU frequency "
			       "%u MHz ***" FS_COLOR_NORMAL "\n", mhz);
			oc_build_json(json, sizeof(json), false, -EINVAL,
				      enabled, configured, current);
			oc_respond(response, json);
			return;
		}

		snprintf(mhz_str, sizeof(mhz_str), "%u", mhz);
		env_set(ENV_OC_ENABLED, "1");
		env_set(ENV_OC_MHZ, mhz_str);

		printf("\n" FS_COLOR_PROMPT "*** Saved delayed CPU frequency: "
		       "%u MHz ***" FS_COLOR_NORMAL "\n", mhz);

		enabled = true;
		configured = mhz;
	} else {
		oc_build_json(json, sizeof(json), false, -EINVAL, enabled,
			      configured, current);
		oc_respond(response, json);
		return;
	}

	oc_build_json(json, sizeof(json), true, error, enabled, configured,
		      current);
	oc_respond(response, json);
}

/* ------------------------------------------------------------------ */
/* /params  -  Config / Bdata / Factory MAC editor                    */
/* ------------------------------------------------------------------ */

struct param_section_desc {
	const char *name;
	uint64_t off;
	size_t size;
};

/* Populated from the live MTD table by xiaomi_resolve_regions(). */
static struct param_section_desc param_sections[2];

#define PARAM_SECTION_COUNT	ARRAY_SIZE(param_sections)

static void params_respond(struct httpd_response *response, char *json)
{
	/*
	 * The buffer is handed to the HTTP layer and released again when the
	 * connection closes, so it is stashed in session_data.
	 */
	response->status = HTTP_RESP_STD;
	response->data = json;
	response->size = strlen(json);
	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "application/json";
	response->session_data = json;
}

/*
 * Build the full state document: both parameter records (content plus CRC
 * status) and the four interface MACs with a layout verdict.
 */
static char *xiaomi_build_params_json(void *flash,
				     const struct xiaomi_regions *reg)
{
	u8 *rec;
	u8 *factory = NULL;
	char *json;
	char mac_rf1[20] = "", mac_rf2[20] = "", mac_lan[20] = "", mac_wan[20] = "";
	bool layout_ok = false;
	size_t cap = 16384;
	unsigned i;

	if (!flash)
		return NULL;

	rec = malloc(XIAOMI_BDATA_SIZE);
	factory = malloc(XIAOMI_FACTORY_SIZE);
	json = malloc(cap);

	if (!rec || !factory || !json) {
		free(rec);
		free(factory);
		free(json);
		return NULL;
	}

	if (!mtk_board_flash_read(flash, reg->factory_off,
				  reg->factory_size, factory)) {
		u8 wan[6], lan[6], rf1[6], rf2[6];

		memcpy(wan, factory + XIAOMI_MAC_WAN_OFF, 6);
		memcpy(lan, factory + XIAOMI_MAC_LAN_OFF, 6);
		memcpy(rf1, factory + XIAOMI_MAC_RF1_OFF, 6);
		memcpy(rf2, factory + XIAOMI_MAC_RF2_OFF, 6);

		format_mac(mac_rf1, sizeof(mac_rf1), rf1);
		format_mac(mac_rf2, sizeof(mac_rf2), rf2);
		format_mac(mac_lan, sizeof(mac_lan), lan);
		format_mac(mac_wan, sizeof(mac_wan), wan);

		layout_ok = macs_layout_ok(wan, lan, rf1, rf2) ? true : false;
	}

	i = snprintf(json, cap, "{\"ok\":true,\"error\":0");

	for (i = 0; i < PARAM_SECTION_COUNT; i++) {
		const struct param_section_desc *d = &param_sections[i];
		char *text;
		char *line;
		int used;

		text = malloc(d->size + 1);
		if (!text)
			break;

		memset(text, 0, d->size + 1);

		if (mtk_board_flash_read(flash, d->off, d->size, rec))
			used = 0;
		else
			used = param_record_parse(rec, d->size, text,
						  d->size + 1);

		snprintf(json + strlen(json), cap - strlen(json),
			 ",\"%s\":{\"read_error\":false,\"crc_ok\":%s,"
			 "\"offset\":\"0x%llx\",\"record_size\":%zu,\"entries\":[",
			 d->name, used > 0 ? "true" : "false",
			 (unsigned long long)d->off, d->size);

		/* Emit one JSON object per key=value line */
		for (line = text; *line; ) {
			char *eol = strchr(line, '\n');
			char *eq;
			char key[800], val[1200];

			if (eol)
				*eol = '\0';

			eq = strchr(line, '=');
			if (eq) {
				*eq = '\0';
				json_escape_x(key, sizeof(key), line);
				json_escape_x(val, sizeof(val), eq + 1);

				snprintf(json + strlen(json),
					 cap - strlen(json),
					 "{\"key\":\"%s\",\"value\":\"%s\"},",
					 key, val);
			}

			if (!eol)
				break;
			line = eol + 1;
		}

		/* Drop the trailing comma left by the last entry, if any */
		{
			size_t len = strlen(json);

			if (len && json[len - 1] == ',')
				json[len - 1] = '\0';
		}

		snprintf(json + strlen(json), cap - strlen(json), "]}");
		free(text);
	}

	snprintf(json + strlen(json), cap - strlen(json),
		 ",\"macs\":{\"read_error\":false,\"layout_ok\":%s,"
		 "\"rf1\":\"%s\",\"rf2\":\"%s\",\"lan\":\"%s\",\"wan\":\"%s\"}}",
		 layout_ok ? "true" : "false", mac_rf1, mac_rf2, mac_lan,
		 mac_wan);

	free(rec);
	free(factory);

	return json;
}

static int params_write_env(void *flash, const char *target, char *text)
{
	const struct param_section_desc *d = NULL;
	u8 *rec;
	size_t text_len = 0;
	unsigned i;
	int ret;

	for (i = 0; i < PARAM_SECTION_COUNT; i++) {
		if (param_sections[i].name &&
		    !strcmp(param_sections[i].name, target)) {
			d = &param_sections[i];
			break;
		}
	}

	if (!d)
		return -EINVAL;

	if (urldecode_inplace(text)) {
		printf(FS_COLOR_ERROR "*** Malformed percent-encoding in %s ***"
		       FS_COLOR_NORMAL "\n", d->name);
		return -EINVAL;
	}

	if (param_text_validate(text, d->size, &text_len)) {
		printf(FS_COLOR_ERROR "*** Invalid %s parameter table ***"
		       FS_COLOR_NORMAL "\n", d->name);
		return -EINVAL;
	}

	rec = malloc(d->size);
	if (!rec)
		return -ENOMEM;

	if (param_record_build(text, text_len, rec, d->size)) {
		free(rec);
		return -EINVAL;
	}

	printf("\n" FS_COLOR_PROMPT "*** Writing Xiaomi %s parameter record "
	       "with CRC verification ***" FS_COLOR_NORMAL "\n", d->name);

	ret = xiaomi_flash_erase_write_verify(flash, d->off, d->size, rec);

	free(rec);

	if (ret) {
		printf(FS_COLOR_ERROR "*** Parameter record write failed ***"
		       FS_COLOR_NORMAL "\n");
		return ret;
	}

	printf(FS_COLOR_PROMPT "*** Parameter record written and verified ***"
	       FS_COLOR_NORMAL "\n");
	return 0;
}

static int params_write_macs(void *flash, struct httpd_request *request,
			     const struct xiaomi_regions *reg)
{
	u8 *factory;
	u8 macs[4][6];
	static const char *ids[] = { "rf1", "rf2", "lan", "wan" };
	static const uint32_t offs[] = {
		XIAOMI_MAC_RF1_OFF, XIAOMI_MAC_RF2_OFF,
		XIAOMI_MAC_LAN_OFF, XIAOMI_MAC_WAN_OFF
	};
	char *scratch = NULL;
	int i, ret = 0;

	/* Parse and validate before touching flash */
	for (i = 0; i < 4; i++) {
		struct httpd_form_value *v = httpd_request_find_value(request,
								      ids[i]);
		char tmp[32];

		if (!v || !v->data)
			return -EINVAL;

		if (!v->data || strlen(v->data) > 3 * sizeof(tmp))
			return -EINVAL;

		strncpy(tmp, v->data, sizeof(tmp) - 1);
		tmp[sizeof(tmp) - 1] = '\0';

		if (urldecode_inplace(tmp) || parse_mac(tmp, macs[i]))
			return -EINVAL;
	}

	/* WAN, LAN, RF1, RF2 must be consecutive */
	if (!macs_layout_ok(macs[3], macs[2], macs[0], macs[1])) {
		printf(FS_COLOR_ERROR "*** MAC addresses are not a valid "
		       "consecutive set ***" FS_COLOR_NORMAL "\n");
		return -EINVAL;
	}

	factory = malloc(reg->factory_size);
	scratch = malloc(reg->factory_size);
	if (!factory || !scratch) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Only the four MAC fields are touched; every other byte of the
	 * factory area holds Wi-Fi calibration data and is copied through
	 * unchanged.
	 */
	if (mtk_board_flash_read(flash, reg->factory_off, reg->factory_size,
				 factory)) {
		ret = -EIO;
		goto out;
	}

	memcpy(scratch, factory, reg->factory_size);

	for (i = 0; i < 4; i++)
		memcpy(scratch + offs[i], macs[i], 6);

	printf("\n" FS_COLOR_PROMPT "*** Writing Xiaomi Factory MAC fields "
	       "while preserving calibration ***" FS_COLOR_NORMAL "\n");

	ret = xiaomi_flash_erase_write_verify(flash, reg->factory_off,
					       reg->factory_size, scratch);
	if (ret) {
		printf(FS_COLOR_ERROR "*** Factory MAC write failed ***"
		       FS_COLOR_NORMAL "\n");
		goto out;
	}

	printf(FS_COLOR_PROMPT "*** Factory MAC fields written and verified ***"
	       FS_COLOR_NORMAL "\n");

out:
	free(factory);
	free(scratch);
	return ret;
}

static void params_fail_respond(struct httpd_response *response,
				const char *why)
{
	static const char * const msgs[] = {
		"{\"ok\":false,\"error\":\"storage\"}",
		"{\"ok\":false,\"error\":\"layout\"}",
	};
	const char *msg = msgs[0];

	if (why && !strcmp(why, "layout"))
		msg = msgs[1];

	response->status = HTTP_RESP_STD;
	response->data = (void *)msg;
	response->size = strlen(msg);
	response->info.code = 500;
	response->info.connection_close = 1;
	response->info.content_type = "application/json";
}

void params_handler(enum httpd_uri_handler_status status,
		    struct httpd_request *request,
		    struct httpd_response *response)
{
	char *json;
	void *flash;
	struct httpd_form_value *action;
	struct xiaomi_regions reg;
	int ret = 0;

	if (status == HTTP_CB_CLOSED) {
		free(response->session_data);
		return;
	}

	if (status != HTTP_CB_NEW)
		return;

	flash = mtk_board_get_flash_dev();
	if (!flash) {
		params_fail_respond(response, "storage");
		return;
	}

	/*
	 * Resolve the stock records from the live MTD table before anything is
	 * read or written.  On a board whose mtdparts does not match the stock
	 * layout this fails, and the page reports the problem instead of
	 * touching flash.
	 */
	if (xiaomi_resolve_regions(&reg)) {
		params_fail_respond(response, "layout");
		return;
	}

	param_sections[0].name = "config";
	param_sections[0].off = reg.config_off;
	param_sections[0].size = reg.config_size;
	param_sections[1].name = "bdata";
	param_sections[1].off = reg.bdata_off;
	param_sections[1].size = reg.bdata_size;

	if (request->method == HTTP_POST) {
		action = httpd_request_find_value(request, "action");

		if (action && !strcmp(action->data, "save_env")) {
			struct httpd_form_value *target;
			struct httpd_form_value *entries;

			target = httpd_request_find_value(request, "target");
			entries = httpd_request_find_value(request, "entries");

			if (target && entries && entries->data)
				ret = params_write_env(flash, target->data,
						       (char *)entries->data);
		} else if (action && !strcmp(action->data, "save_macs")) {
			ret = params_write_macs(flash, request, &reg);
		} else {
			ret = -EINVAL;
		}
	}

	json = xiaomi_build_params_json(flash, &reg);
	if (!json) {
		response->status = HTTP_RESP_STD;
		response->data = ret ? "{\"ok\":false,\"error\":\"write\"}"
				     : "{\"ok\":false,\"error\":\"storage\"}";
		response->size = strlen(response->data);
		response->info.code = 500;
		response->info.connection_close = 1;
		response->info.content_type = "application/json";
		return;
	}

	/* Surface a write failure even though the state read-back succeeded */
	if (ret) {
		size_t len = strlen(json);

		if (len > 0 && !strncmp(json, "{\"ok\":true", 10)) {
			char *p = strstr(json, "\"error\":0");

			if (p)
				memcpy(p + 8, "\"write\"", 7);
		}
	}

	params_respond(response, json);
}

/* ------------------------------------------------------------------ */
/* Xiaomi stock (HDR1) firmware restore                              */
/* ------------------------------------------------------------------ */

/*
 * Erase one partition, program an image into it and read the result back.
 * The whole image is verified byte-for-byte, so a NAND write error is caught
 * here rather than surfacing as an unbootable device later.
 */
static int xiaomi_write_partition(void *flash, const char *name,
				  uint64_t part_off, uint64_t part_size,
				  uint32_t erase_size, const void *src,
				  uint32_t len)
{
	uint32_t to_erase;
	int ret;

	to_erase = ALIGN(len, erase_size);

	if (to_erase > part_size) {
		printf("\n" FS_COLOR_ERROR "*** Error: image is larger than MTD "
		       "partition '%s' ***" FS_COLOR_NORMAL "\n", name);
		return CMD_RET_FAILURE;
	}

	printf("\nErasing '%s' at 0x%llx, size 0x%x ... ",
	       name, part_off, to_erase);

	ret = mtk_board_flash_erase(flash, part_off, to_erase);
	if (ret) {
		printf("Fail\n" FS_COLOR_ERROR "*** Flash erasure [%llx-%llx] "
		       "failed! ***" FS_COLOR_NORMAL "\n",
		       part_off, part_off + to_erase - 1);
		return CMD_RET_FAILURE;
	}
	printf("OK\n");

	ret = xiaomi_flash_erase_write_verify(flash, part_off, len, src);
	if (ret) {
		printf("\n" FS_COLOR_ERROR "*** Flash program or verification of "
		       "'%s' failed! ***" FS_COLOR_NORMAL "\n", name);
		return CMD_RET_FAILURE;
	}

	return CMD_RET_SUCCESS;
}

/*
 * A member of the official Xiaomi (HDR1) firmware bundle.
 *
 * An HDR1 firmware file is a tar archive.  The AC2100 build of this
 * bootloader walks those entries by name, so the names are the contract:
 *
 *   xiaoqiang_version  small text file, contains the "option HARDWARE 'RM2100'"
 *                      line used to identify the model
 *   uImage.bin         the kernel, a uImage with the usual 0x27051956 magic
 *   root.ubi           the raw UBIFS image that is written to the ubi
 *                      partition (or mounted as the UBI volume table)
 *
 * Nothing about the archive is trusted before this runs: the header magic,
 * the entry sizes and the kernel's own header and payload CRC are all
 * verified first, and any mismatch aborts before a single byte of flash is
 * erased.
 */
struct stock_entry {
	const void *data;
	size_t size;
};

/* 512-byte tar record; the format has no padding requirements beyond it. */
#define TAR_BLOCK	512

struct tar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char chksum[8];
	char typeflag[1];
	char linkname[100];
	char magic[6];
	char version[2];
	char user[32];
	char group[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
	char pad[12];
};

/* Parse an octal, possibly space/NUL padded, tar numeric field. */
static int tar_octal(const char *field, size_t len, unsigned long long *out)
{
	unsigned long long v = 0;
	size_t i = 0;
	int seen = 0;

	if (!field || !len)
		return -EINVAL;

	/* skip leading spaces/NULs */
	while (i < len && (field[i] == ' ' || field[i] == '\0'))
		i++;

	for (; i < len; i++) {
		char c = field[i];

		if (c == ' ' || c == '\0')
			break;

		if (c < '0' || c > '7')
			return -EINVAL;

		/* guard against overflow on a hostile header */
		if (v > (0xffffffffffffffffULL - 7) / 8)
			return -EINVAL;

		v = v * 8 + (unsigned)(c - '0');
		seen = 1;
	}

	if (!seen)
		return -EINVAL;

	*out = v;
	return 0;
}

static int tar_header_sane(const struct tar_header *th, unsigned long long size)
{
	unsigned long long sum = 0;
	unsigned long long stored;
	size_t i;
	int res;

	if (memcmp(th->magic, "ustar", 5))
		return -EINVAL;

	if (tar_octal(th->size, sizeof(th->size), &stored))
		return -EINVAL;

	if (stored != size)
		return -EINVAL;

	/*
	 * Verify the header checksum.  The checksum field itself counts as
	 * spaces, and a signed char interpretation is what tar uses.
	 */
	for (i = 0; i < sizeof(*th); i++) {
		if (i >= offsetof(struct tar_header, chksum) &&
		    i < offsetof(struct tar_header, chksum) + sizeof(th->chksum))
			sum += ' ';
		else
			sum += (unsigned char)th->name[i];
	}

	if (tar_octal(th->chksum, sizeof(th->chksum), &stored))
		return -EINVAL;

	if (sum != stored)
		return -EINVAL;

	return 0;
}

/*
 * Locate one member of the bundle.  Only regular files are considered and the
 * payload must lie wholly inside the archive.
 */
static int stock_find_member(const void *image, size_t image_size,
			     const char *want, struct stock_entry *out)
{
	const u8 *base = (const u8 *)image;
	size_t off = 0;

	while (off + TAR_BLOCK <= image_size) {
		const struct tar_header *th;
		unsigned long long size;
		size_t padded;

		/* an all-zero header marks the end of the archive */
		{
			size_t z;
			int blank = 1;

			for (z = 0; z < TAR_BLOCK; z++) {
				if (base[off + z]) {
					blank = 0;
					break;
				}
			}

			if (blank)
				return -ENOENT;
		}

		th = (const struct tar_header *)(base + off);

		if (tar_octal(th->size, sizeof(th->size), &size))
			return -EINVAL;

		/* reject a corrupt header before trusting its size */
		if (tar_header_sane(th, size))
			return -EINVAL;

		off += TAR_BLOCK;

		if (size > image_size - off)
			return -EINVAL;

		if (th->typeflag[0] == '\0' || th->typeflag[0] == '0') {
			if (!strncmp(th->name, want, sizeof(th->name)) &&
			    size <= image_size - off) {
				out->data = base + off;
				out->size = (size_t)size;
				return 0;
			}
		}

		/* skip the payload, rounded up to a block */
		padded = (size_t)((size + TAR_BLOCK - 1) & ~(unsigned long long)(TAR_BLOCK - 1));

		if (padded > image_size - off)
			return -EINVAL;

		off += padded;
	}

	return -ENOENT;
}

/*
 * The model is carried as a device string, not a numeric id, in the
 * xiaoqiang_version file.  Refuse anything that is not one of the routers
 * this bootloader supports, checked before any flash is touched.
 */
static const char *const xiaomi_stock_models[] = {
	"RM2100",	/* Redmi AC2100 */
	"R1900",	/* Mi Router 4  */
	"R2300",	/* Mi Router 4C */
	NULL
};

static int xiaomi_stock_model_ok(const char *text, size_t len)
{
	unsigned i;

	for (i = 0; xiaomi_stock_models[i]; i++) {
		size_t mlen = strlen(xiaomi_stock_models[i]);

		if (len >= mlen && !memcmp(text, xiaomi_stock_models[i], mlen))
			return (int)i;
	}

	return -1;
}

/*
 * Validate an official Xiaomi (HDR1) firmware bundle.
 *
 * Returns 0 and fills in the extracted members, or -EINVAL.  Nothing is
 * written to flash by this function.
 */
static int xiaomi_validate_stock_image(const void *data, size_t size,
				       struct stock_entry *kernel,
				       struct stock_entry *rootfs,
				       const char **model)
{
	struct stock_entry version;
	const image_header_t *kh;
	const char *text;
	int idx;

	memset(kernel, 0, sizeof(*kernel));
	memset(rootfs, 0, sizeof(*rootfs));

	if (stock_find_member(data, size, "uImage.bin", kernel))
		return -EINVAL;

	if (stock_find_member(data, size, "root.ubi", rootfs))
		return -EINVAL;

	if (stock_find_member(data, size, "xiaoqiang_version", &version))
		return -EINVAL;

	/* the model line is short; refuse anything unreasonable */
	if (!version.size || version.size > 4096)
		return -EINVAL;

	text = (const char *)version.data;
	idx = xiaomi_stock_model_ok(text, version.size);
	if (idx < 0)
		return -EINVAL;

	/*
	 * Validate the kernel: real uImage magic, the declared payload must
	 * fit inside the extracted member, and the data CRC must match.
	 */
	if (kernel->size < sizeof(image_header_t))
		return -EINVAL;

	kh = (const image_header_t *)kernel->data;

	if (image_get_magic(kh) != IH_MAGIC)
		return -EINVAL;

	if (image_get_size(kh) + sizeof(image_header_t) > kernel->size)
		return -EINVAL;

	if (crc32(0, (const u8 *)kernel->data + sizeof(image_header_t),
		  image_get_size(kh)) != image_get_dcrc(kh))
		return -EINVAL;

	if (rootfs->size < 8)
		return -EINVAL;

	/* a raw UBIFS image starts with the "UBI#" magic */
	if (memcmp(rootfs->data, "UBI#", 4))
		return -EINVAL;

	if (model)
		*model = xiaomi_stock_models[idx];

	return 0;
}

/*
 * Restore an official Xiaomi image.
 *
 * The bundle holds a kernel and a raw root.ubi.  The kernel goes to the
 * 'kernel' partition, the root filesystem is programmed into the 'ubi'
 * partition.  Config, Bdata, Factory and OBR are deliberately left
 * untouched so Wi-Fi calibration and the device identity survive.
 *
 * Every partition offset is resolved from the live MTD table at run time;
 * a single compiled-in address would be wrong on any board whose mtdparts
 * differs from the stock one, and would silently destroy the U-Boot
 * environment.
 */
int write_xiaomi_stock_failsafe(size_t data_addr, uint32_t data_size)
{
	void *flash;
	struct stock_entry kernel, rootfs;
	const char *model = NULL;
	uint64_t k_off, k_size, ubi_off, ubi_size, tmp;
	uint32_t erase_size;
	int ret;

	flash = mtk_board_get_flash_dev();
	if (!flash)
		return CMD_RET_FAILURE;

	erase_size = mtk_board_get_flash_erase_size(flash);

	ret = xiaomi_validate_stock_image((const void *)data_addr, data_size,
					  &kernel, &rootfs, &model);
	if (ret) {
		printf("\n" FS_COLOR_ERROR "*** Invalid or wrong-model Xiaomi "
		       "HDR1 firmware ***" FS_COLOR_NORMAL "\n");
		return CMD_RET_FAILURE;
	}

	printf("\n" FS_COLOR_PROMPT "*** Validated Xiaomi HDR1 stock firmware "
	       "(device %s) ***" FS_COLOR_NORMAL "\n", model);

	/*
	 * Resolve the target partitions at runtime; never trust compiled-in
	 * offsets here.
	 */
	if (get_mtd_part_info("kernel", &k_off, &k_size) ||
	    get_mtd_part_info("ubi", &ubi_off, &ubi_size)) {
		printf("\n" FS_COLOR_ERROR "*** Required MTD partitions "
		       "'kernel' and 'ubi' were not found ***" FS_COLOR_NORMAL "\n");
		return CMD_RET_FAILURE;
	}

	/* the stock bootloader needs the two areas adjacent */
	if (k_off + k_size != ubi_off) {
		printf("\n" FS_COLOR_ERROR "*** NAND geometry does not match "
		       "Xiaomi AC2100 stock layout ***" FS_COLOR_NORMAL "\n");
		return CMD_RET_FAILURE;
	}

	/* refuse before erasing if either component cannot fit */
	if (kernel.size > k_size || rootfs.size > ubi_size) {
		printf("\n" FS_COLOR_ERROR "*** Xiaomi stock firmware exceeds "
		       "its MTD partitions (kernel 0x%zx/0x%llx, root 0x%zx/0x%llx) "
		       "***" FS_COLOR_NORMAL "\n", kernel.size,
		       (unsigned long long)k_size, rootfs.size,
		       (unsigned long long)ubi_size);
		return CMD_RET_FAILURE;
	}

	/* both areas must start on an erase boundary */
	tmp = k_off;
	if (do_div(tmp, erase_size)) {
		printf("\n" FS_COLOR_ERROR "*** MTD partition 'kernel' does not "
		       "start on erase boundary! ***" FS_COLOR_NORMAL "\n");
		return CMD_RET_FAILURE;
	}

	tmp = ubi_off;
	if (do_div(tmp, erase_size)) {
		printf("\n" FS_COLOR_ERROR "*** MTD partition 'ubi' does not "
		       "start on erase boundary! ***" FS_COLOR_NORMAL "\n");
		return CMD_RET_FAILURE;
	}

	printf("\n" FS_COLOR_PROMPT "*** Preserving bootloader, Config, Bdata, "
	       "Factory and OBR. ***" FS_COLOR_NORMAL "\n");
	printf("\n" FS_COLOR_PROMPT "*** Kernel: 0x%zx bytes, raw root.ubi: "
	       "0x%zx bytes ***" FS_COLOR_NORMAL "\n", kernel.size, rootfs.size);

	/* program the kernel, verifying every byte back */
	ret = xiaomi_write_partition(flash, "kernel", k_off, k_size,
				     erase_size, kernel.data, kernel.size);
	if (ret)
		return CMD_RET_FAILURE;

	/*
	 * The UBI area is replaced wholesale: a stock root.ubi carries its
	 * own volume table, so no volume can be preserved across this
	 * write.  Config, Bdata, Factory and OBR live outside it and are
	 * never touched.
	 */
	ret = xiaomi_write_partition(flash, "ubi", ubi_off, ubi_size,
				     erase_size, rootfs.data, rootfs.size);
	if (ret)
		return CMD_RET_FAILURE;

	printf("\n" FS_COLOR_PROMPT "*** Xiaomi stock firmware restored and "
	       "verified! ***" FS_COLOR_NORMAL "\n");

	return CMD_RET_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Delayed CPU frequency, applied on the next OS boot                 */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_MACH_MT7621
/*
 * Apply a frequency requested by the failsafe Web UI.
 *
 * U-Boot cannot retune the MT7621 CPU PLL while it is executing from DRAM
 * (the DDR controller shares the MEMPLL domain), so the setting is recorded
 * in the environment and honoured on the next boot through the legacy DRAM
 * init path, which patches RG_MEPL_FBDIV before DRAM comes up.  This hook
 * reports what will be used and validates the stored value.
 */
void xiaomi_apply_saved_cpu_freq(void)
{
	const char *en = env_get(ENV_OC_ENABLED);
	const char *mhz = env_get(ENV_OC_MHZ);
	unsigned target;

	if (!en || strcmp(en, "1") || !mhz)
		return;

	target = simple_strtoul(mhz, NULL, 10);

	if (!oc_is_valid_mhz(target)) {
		printf("\n" FS_COLOR_ERROR "*** CPU overclock rejected; booting "
		       "at safe frequency ***" FS_COLOR_NORMAL "\n");
		env_set(ENV_OC_ENABLED, "0");
		env_set(ENV_OC_MHZ, "0");
		return;
	}

	if (target == oc_safe_mhz()) {
		printf("\n" FS_COLOR_PROMPT "*** CPU overclock disabled; safe "
		       "frequency will be used ***" FS_COLOR_NORMAL "\n");
		env_set(ENV_OC_ENABLED, "0");
		env_set(ENV_OC_MHZ, "0");
		return;
	}

	printf("\n" FS_COLOR_PROMPT "*** Applying delayed CPU overclock: %u MHz "
	       "***" FS_COLOR_NORMAL "\n", target);
}
#else
void xiaomi_apply_saved_cpu_freq(void)
{
}
#endif

/*
 * Exported for the failsafe boot path.  A stored value that is out of range
 * is cleared here so a bad setting cannot survive indefinitely.
 */
void xiaomi_sanitize_cpu_freq_env(void)
{
	const char *en = env_get(ENV_OC_ENABLED);
	const char *mhz = env_get(ENV_OC_MHZ);
	unsigned target;

	if (!en || strcmp(en, "1") || !mhz)
		return;

	target = simple_strtoul(mhz, NULL, 10);

	if (!oc_is_valid_mhz(target)) {
		printf("\n" FS_COLOR_ERROR "*** Warning: unable to restore safe "
		       "CPU frequency ***" FS_COLOR_NORMAL "\n");
		env_set(ENV_OC_ENABLED, "0");
		env_set(ENV_OC_MHZ, "0");
	}
}
