/*
 * Randomised / adversarial input sweep for the HDR1 bundle parser.
 *
 * The unit tests cover the cases we thought of.  This mutates a valid
 * tar bundle and a valid uImage systematically, and feeds truncated,
 * oversized and randomly corrupted buffers, to check the parser always
 * either returns an error or returns data that lies inside the input.
 * It runs under ASan/UBSan in CI, so a read past the end of the buffer
 * aborts the run instead of passing quietly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

typedef uint8_t u8;
#define EINVAL 22
#define ENOENT 2

/* ---- copies under test (see check_copy_sync.py) ---- */

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

	while (i < len && (field[i] == ' ' || field[i] == '\0'))
		i++;

	for (; i < len; i++) {
		char c = field[i];

		if (c == ' ' || c == '\0')
			break;

		if (c < '0' || c > '7')
			return -EINVAL;

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

		padded = (size_t)((size + TAR_BLOCK - 1) & ~(unsigned long long)(TAR_BLOCK - 1));

		if (padded > image_size - off)
			return -EINVAL;

		off += padded;
	}

	return -ENOENT;
}

/* ------------------------------------------------------------------ */

static void tar_num(char *dst, size_t n, unsigned long long v)
{
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
	tar_num(th->size, sizeof(th->size), size);
	snprintf(th->mtime, sizeof(th->mtime), "00000000000");
	th->typeflag[0] = '0';
	memcpy(th->magic, "ustar", 5);
	memcpy(th->version, "00", 2);

	memset(th->chksum, ' ', sizeof(th->chksum));
	for (size_t i = 0; i < sizeof(*th); i++)
		sum += (unsigned char)((const char *)th)[i];
	tar_num(th->chksum, sizeof(th->chksum), sum);
}

static unsigned rng_state = 12345;

static unsigned rng(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return (rng_state >> 8) & 0x7fffffff;
}

/* Build a plausible HDR1 bundle into buf, return its length. */
static size_t build_bundle(u8 *buf, size_t cap)
{
	struct tar_header th;
	size_t off = 0;

	if (cap < 4 * TAR_BLOCK)
		return 0;

	/* xiaoqiang_version */
	tar_build(&th, "xiaoqiang_version", 32);
	memcpy(buf + off, &th, sizeof(th));
	off += TAR_BLOCK;
	memset(buf + off, 0, 32);
	off += TAR_BLOCK;

	/* uImage.bin, 600 bytes of payload */
	tar_build(&th, "uImage.bin", 600);
	memcpy(buf + off, &th, sizeof(th));
	off += TAR_BLOCK;
	memset(buf + off, 0x5A, 600);
	off += 1024;

	/* root.ubi, 300 bytes */
	tar_build(&th, "root.ubi", 300);
	memcpy(buf + off, &th, sizeof(th));
	off += TAR_BLOCK;
	memset(buf + off, 0x33, 300);
	off += 512;

	/* end of archive */
	memset(buf + off, 0, 2 * TAR_BLOCK);
	off += 2 * TAR_BLOCK;

	return off;
}

int main(void)
{
	enum { CAP = 8192 };
	static u8 work[CAP];
	u8 *buf = malloc(CAP);
	int accepted = 0, rejected = 0, escapes = 0;
	int i;

	if (!buf) {
		fprintf(stderr, "oom\n");
		return 2;
	}

	printf("=== HDR1 parser fuzz sweep ===\n");

	/* 1) truncations of a valid bundle at every length */
	{
		size_t full = build_bundle(work, CAP);

		for (size_t len = 0; len <= full; len++) {
			struct stock_entry t;

			if (stock_find_member(work, len, "uImage.bin", &t) == 0) {
				accepted++;
				/* a returned member must lie inside the buffer */
				if ((const u8 *)t.data < work ||
				    (const u8 *)t.data + t.size > work + len) {
					printf("  FAIL: accepted member escapes a "
					       "%zu-byte buffer\n", len);
					escapes++;
				}
			} else {
				rejected++;
			}
		}
		printf("  truncation sweep: %d accepted, %d rejected\n",
		       accepted, rejected);
	}

	/* 2) single-byte corruption across the whole bundle */
	accepted = rejected = 0;
	{
		size_t full = build_bundle(work, CAP);

		for (i = 0; i < (int)full; i++) {
			u8 saved = work[i];

			for (int v = 0; v < 256; v += 17) {
				work[i] = (u8)v;
				struct stock_entry t;

				if (stock_find_member(work, full, "uImage.bin",
						      &t) == 0) {
					accepted++;
					if ((const u8 *)t.data + t.size >
					    work + full) {
						printf("  FAIL: corrupt member "
						       "escapes at %d\n", i);
						escapes++;
					}
				} else {
					rejected++;
				}
			}
			work[i] = saved;
		}
		printf("  corruption sweep: %d accepted, %d rejected\n",
		       accepted, rejected);
	}

	/* 3) random buffers, including ones that look tar-ish */
	accepted = rejected = 0;
	for (i = 0; i < 20000; i++) {
		size_t len = (size_t)(rng() % (CAP - 1)) + 1;
		struct stock_entry t;

		for (size_t k = 0; k < len; k++)
			buf[k] = (u8)rng();

		/* every so often, plant a ustar magic at a block boundary */
		if ((rng() & 3) == 0 && len >= TAR_BLOCK) {
			size_t bo = ((size_t)rng() % (len / TAR_BLOCK)) * TAR_BLOCK;
			if (bo + TAR_BLOCK <= len)
				memcpy(buf + bo, "ustar", 5);
		}

		if (stock_find_member(buf, len, "uImage.bin", &t) == 0) {
			accepted++;
			if ((const u8 *)t.data < buf ||
			    (const u8 *)t.data + t.size > buf + len) {
				printf("  FAIL: random member escapes "
				       "(iter %d, len %zu)\n", i, len);
				escapes++;
			}
		} else {
			rejected++;
		}
	}
	printf("  random sweep: %d accepted, %d rejected\n", accepted, rejected);

	free(buf);

	if (escapes) {
		printf("\nFAIL: %d escaped member(s)\n", escapes);
		return 1;
	}
	printf("\nno member ever escaped its input buffer\n");
	return 0;
}
