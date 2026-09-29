/*
 * Host-side unit tests for the pure-logic helpers in failsafe_xiaomi.c.
 *
 * These functions do not touch hardware: tar parsing, CRC and magic
 * validation, MAC parsing/formatting, URL decoding and the JSON escaper.
 * They are the parts that a malformed upload can attack, so they are
 * compiled here for the host and driven with adversarial input.
 *
 * The bodies below are copied verbatim from
 * failsafe/failsafe_xiaomi.c.  tests/run_xiaomi_tests.sh checks that the
 * copies still match, so a change to the real file fails the build instead
 * of silently testing stale code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;

#define EINVAL 22
#define ENOENT 2

static int tests_run;
static int tests_failed;

#define CHECK(cond, ...) do {                                            \
	tests_run++;                                                      \
	if (cond) {                                                        \
		printf("  ok   ");                                           \
		printf(__VA_ARGS__);                                         \
		printf("\n");                                                \
	} else {                                                         \
		tests_failed++;                                              \
		printf("  FAIL ");                                           \
		printf(__VA_ARGS__);                                         \
		printf("   (%s:%d)\n", __FILE__, __LINE__);                   \
	}                                                                \
} while (0)

/* ------------------------------------------------------------------ */
/* copied from failsafe_xiaomi.c                                      */
/* ------------------------------------------------------------------ */

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

	/*
	 * Broadcast only when every byte is 0xff.  ANDing the bytes together
	 * would instead test whether they share any bit, which rejects most
	 * ordinary addresses (64:09:80:aa:bb:cc has 0x64 & 0x09 == 0).
	 */
	if (out[0] == 0xff && out[1] == 0xff && out[2] == 0xff &&
	    out[3] == 0xff && out[4] == 0xff && out[5] == 0xff)
		return -EINVAL;			/* broadcast */

	return 0;
}

static void format_mac(char *dst, size_t dst_sz, const u8 mac[6])
{
	snprintf(dst, dst_sz, "%02x:%02x:%02x:%02x:%02x:%02x",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

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

struct stock_entry {
	const void *data;
	size_t size;
};

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
	/*
	 * Index through the whole record as raw bytes.  Walking it as
	 * th->name[i] would be out of bounds past the 100-byte name field,
	 * which is far smaller than the 512-byte block.
	 */
	for (i = 0; i < sizeof(*th); i++) {
		if (i >= offsetof(struct tar_header, chksum) &&
		    i < offsetof(struct tar_header, chksum) + sizeof(th->chksum))
			sum += ' ';
		else
			sum += (unsigned char)((const char *)th)[i];
	}

	if (tar_octal(th->chksum, sizeof(th->chksum), &stored))
		return -EINVAL;

	if (sum != stored)
		return -EINVAL;

	return 0;
}

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

#define OC_MIN_MHZ	400
#define OC_MAX_MHZ	1200
#define OC_STEP_MHZ	40

static bool oc_is_valid_mhz(unsigned mhz)
{
	if (mhz < OC_MIN_MHZ || mhz > OC_MAX_MHZ)
		return false;
	if (mhz % OC_STEP_MHZ)
		return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* tar builder                                                         */
/* ------------------------------------------------------------------ */

static void tar_num(char *dst, size_t n, unsigned long long v)
{
	/* n-1 octal digits, NUL terminated */
	char tmp[32];
	int len = 0;

	do {
		tmp[len++] = (char)('0' + (v & 7));
		v >>= 3;
	} while (v);

	while (len < (int)n - 1)
		tmp[len++] = '0';

	for (int i = 0; i < (int)n - 1; i++)
		dst[i] = tmp[len - 1 - i];
	dst[n - 1] = '\0';
}

static void tar_build(struct tar_header *th, const char *name, size_t size)
{
	unsigned long long sum = 0;

	memset(th, 0, sizeof(*th));
	snprintf(th->name, sizeof(th->name), "%s", name);
	snprintf(th->mode, sizeof(th->mode), "0000644");
	snprintf(th->uid, sizeof(th->uid), "0000000");
	snprintf(th->gid, sizeof(th->gid), "0000000");
	tar_num(th->size, sizeof(th->size), size);
	snprintf(th->mtime, sizeof(th->mtime), "00000000000");
	th->typeflag[0] = '0';
	memcpy(th->magic, "ustar", 5);
	memcpy(th->version, "00", 2);

	/* checksum: the chksum field counts as spaces */
	memset(th->chksum, ' ', sizeof(th->chksum));
	for (size_t i = 0; i < sizeof(*th); i++)
		sum += (unsigned char)((const char *)th)[i];
	tar_num(th->chksum, sizeof(th->chksum), sum);
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static void test_parse_mac(void)
{
	u8 m[6];
	char buf[32];

	printf("parse_mac / format_mac\n");

	CHECK(parse_mac("64:09:80:aa:bb:cc", m) == 0, "accepts a normal unicast MAC");
	CHECK(m[0] == 0x64 && m[5] == 0xcc, "parses the bytes correctly");

	format_mac(buf, sizeof(buf), m);
	CHECK(!strcmp(buf, "64:09:80:aa:bb:cc"), "format_mac round-trips");

	/* the exact-length check is what keeps the loop in bounds */
	CHECK(parse_mac("", m) == -EINVAL, "rejects empty string");
	CHECK(parse_mac("64:09:80:aa:bb:c", m) == -EINVAL, "rejects 16-char string");
	CHECK(parse_mac("64:09:80:aa:bb:cc:dd", m) == -EINVAL, "rejects 20-char string");
	CHECK(parse_mac("64:09:80:aa:bb:ccdd", m) == -EINVAL, "rejects 19-char string");
	CHECK(parse_mac("zzzzzzzzzzzzzzzzz", m) == -EINVAL, "rejects non-hex");
	CHECK(parse_mac("64-09-80-aa-bb-cc", m) == -EINVAL, "rejects wrong separator");
	CHECK(parse_mac("64:09:80:aa:bb:cc\ntrailing", m) == -EINVAL,
	      "rejects 17-char string with embedded text");
	CHECK(parse_mac("ff:ff:ff:ff:ff:ff", m) == -EINVAL, "rejects all-ones (broadcast)");
	CHECK(parse_mac("00:00:00:00:00:00", m) == -EINVAL, "rejects all-zero");
	CHECK(parse_mac("01:02:03:04:05:06", m) == -EINVAL, "rejects multicast");
	CHECK(parse_mac(NULL, m) == -EINVAL, "rejects NULL");
}

static void test_urldecode(void)
{
	char b[64];
	int rc;

	printf("urldecode_inplace\n");

	snprintf(b, sizeof(b), "config");
	CHECK(urldecode_inplace(b) == 0 && !strcmp(b, "config"), "plain text passes through");

	snprintf(b, sizeof(b), "a+b");
	CHECK(urldecode_inplace(b) == 0 && !strcmp(b, "a b"), "'+' becomes a space");

	snprintf(b, sizeof(b), "%%2Fetc%%2Fpasswd");
	CHECK(urldecode_inplace(b) == 0 && !strcmp(b, "/etc/passwd"),
	      "decodes percent escapes");

	snprintf(b, sizeof(b), "%%");
	rc = urldecode_inplace(b);
	CHECK(rc == -EINVAL, "rejects a truncated escape (not memory safe to accept)");

	snprintf(b, sizeof(b), "%%4");
	rc = urldecode_inplace(b);
	CHECK(rc == -EINVAL, "rejects a 2-char escape");

	snprintf(b, sizeof(b), "%%zz");
	rc = urldecode_inplace(b);
	CHECK(rc == -EINVAL, "rejects non-hex escapes");

	snprintf(b, sizeof(b), "abc%%");
	rc = urldecode_inplace(b);
	CHECK(rc == -EINVAL, "rejects a trailing lone percent");
}

static void test_json_escape(void)
{
	char b[64];
	int n;

	printf("json_escape_x\n");

	n = json_escape_x(b, sizeof(b), "he\"llo\\");
	CHECK(n == 9 && !strcmp(b, "he\\\"llo\\\\"),
	      "escapes quote and backslash");

	n = json_escape_x(b, sizeof(b), "a\nb");
	CHECK(n == 3 && !strcmp(b, "a b"), "control characters become spaces");

	{
		/* a 4-byte destination: 2 chars plus the terminator fit */
		char small[5];

		memset(small, 0x7f, sizeof(small));
		n = json_escape_x(small, 4, "abcdefgh");
		CHECK(n == 2 && small[0] == 'a' && small[1] == 'b' &&
		      small[2] == '\0' && small[3] == 0x7f,
		      "respects a small destination buffer and does not overrun it");
	}

	n = json_escape_x(b, sizeof(b), NULL);
	CHECK(n == 0 && b[0] == '\0', "handles NULL input");

	n = json_escape_x(NULL, 0, "x");
	CHECK(n == 0, "handles NULL destination");
}

static void test_tar_octal(void)
{
	unsigned long long v;

	printf("tar_octal\n");

	CHECK(tar_octal("00000000123", 11, &v) == 0 && v == 83, "parses an octal number");
	CHECK(tar_octal("00000000000", 11, &v) == 0 && v == 0, "parses zero");
	CHECK(tar_octal("  123", 11, &v) == 0 && v == 83, "tolerates leading spaces");
	CHECK(tar_octal("00000000123\0\0\0", 11, &v) == 0 && v == 83,
	      "stops at a NUL terminator");
	CHECK(tar_octal("123456789012345", 15, &v) == -EINVAL,
	      "rejects a non-octal digit");
	CHECK(tar_octal("        ", 8, &v) == -EINVAL, "rejects an all-blank field");
	CHECK(tar_octal("\0\0\0\0\0\0\0\0", 8, &v) == -EINVAL, "rejects an all-NUL field");
	CHECK(tar_octal("", 0, &v) == -EINVAL, "rejects a zero-length field");
	CHECK(tar_octal("7777777777777777777777", 22, &v) == -EINVAL,
	      "rejects a value that would overflow 64 bits");
}

static void test_tar_header_sane(void)
{
	struct tar_header th;
	unsigned long long stored;

	printf("tar_header_sane\n");

	tar_build(&th, "uImage.bin", 1024);
	tar_octal(th.size, sizeof(th.size), &stored);
	CHECK(tar_header_sane(&th, 1024) == 0, "accepts a well-formed header");
	CHECK(tar_header_sane(&th, 1025) == -EINVAL, "rejects a size mismatch");

	th.magic[0] = 'X';
	CHECK(tar_header_sane(&th, 1024) == -EINVAL, "rejects a bad ustar magic");
	th.magic[0] = 'u';

	th.name[0] = 'Z';
	CHECK(tar_header_sane(&th, 1024) == -EINVAL,
	      "rejects a tampered name (checksum mismatch)");
}

static void test_stock_find_member(void)
{
	enum { TOTAL = 512 * 8 };
	u8 *tar = calloc(1, TOTAL);
	struct tar_header th;
	struct stock_entry e;

	printf("stock_find_member\n");

	/* uImage.bin first, then root.ubi, then the end-of-archive marker */
	tar_build(&th, "uImage.bin", 600);
	memcpy(tar, &th, sizeof(th));
	memset(tar + 512, 0xA5, 600);
	memset(tar + 512 + 600, 0, 512 - (600 % 512));

	tar_build(&th, "root.ubi", 300);
	memcpy(tar + 512 + 1024, &th, sizeof(th));
	memset(tar + 512 + 1024 + 512, 0x5A, 300);

	CHECK(stock_find_member(tar, TOTAL, "uImage.bin", &e) == 0 &&
	      e.size == 600, "finds uImage.bin");
	CHECK(stock_find_member(tar, TOTAL, "root.ubi", &e) == 0 &&
	      e.size == 300, "finds root.ubi");
	CHECK(stock_find_member(tar, TOTAL, "xiaoqiang_version", &e) == -ENOENT,
	      "reports a missing member");

	/* a header claiming a size far past the end must be refused */
	tar_build(&th, "huge", 0x7fffffff);
	memcpy(tar + 512 + 1024 + 1024, &th, sizeof(th));
	CHECK(stock_find_member(tar, TOTAL, "huge", &e) != 0,
	      "refuses a size that runs past the buffer");

	/* a corrupt checksum must be caught before the size is trusted */
	tar_build(&th, "corrupt", 16);
	th.name[3] = '!';
	memcpy(tar + 512 + 1024 + 1024, &th, sizeof(th));
	CHECK(stock_find_member(tar, TOTAL, "corrupt", &e) != 0,
	      "refuses a header with a bad checksum");

	free(tar);
}

static void test_model_ok(void)
{
	printf("xiaomi_stock_model_ok\n");

	CHECK(xiaomi_stock_model_ok("RM2100", 6) >= 0, "accepts RM2100");
	CHECK(xiaomi_stock_model_ok("R1900", 5) >= 0, "accepts R1900");
	CHECK(xiaomi_stock_model_ok("R2300", 5) >= 0, "accepts R2300");
	CHECK(xiaomi_stock_model_ok("R3600", 5) < 0, "rejects an unknown model");
	CHECK(xiaomi_stock_model_ok("RM2100", 3) < 0, "rejects a truncated model string");
	CHECK(xiaomi_stock_model_ok("", 0) < 0, "rejects empty text");
}

static void test_oc_range(void)
{
	printf("oc_is_valid_mhz\n");

	CHECK(oc_is_valid_mhz(400), "accepts the minimum");
	CHECK(oc_is_valid_mhz(1200), "accepts the maximum");
	CHECK(oc_is_valid_mhz(880), "accepts a mid value");
	CHECK(!oc_is_valid_mhz(399), "rejects below the minimum");
	CHECK(!oc_is_valid_mhz(1201), "rejects above the maximum");
	CHECK(!oc_is_valid_mhz(0), "rejects zero");
	CHECK(!oc_is_valid_mhz(401), "rejects a value off the step");
}

int main(void)
{
	printf("=== failsafe_xiaomi.c pure-logic tests ===\n\n");

	test_parse_mac();
	test_urldecode();
	test_json_escape();
	test_tar_octal();
	test_tar_header_sane();
	test_stock_find_member();
	test_model_ok();
	test_oc_range();

	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
