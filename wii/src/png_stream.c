#include "png_stream.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define PNG_MAX_WIDTH 2048
#define BPP 4

static const u8 kSignature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

bool WiiPng_size(const u8 *png, size_t len, int *width, int *height)
{
	if (len < 33 || memcmp(png, kSignature, 8) != 0 || memcmp(png + 12, "IHDR", 4) != 0)
		return false;
	*width = (int)be32(png + 16);
	*height = (int)be32(png + 20);
	/* depth 8, colour type 6 (RGBA), deflate, adaptive filtering, no interlace */
	if (png[24] != 8 || png[25] != 6 || png[26] != 0 || png[27] != 0 || png[28] != 0)
		return false;
	return *width > 0 && *width <= PNG_MAX_WIDTH && *height > 0;
}

static int paeth(int a, int b, int c)
{
	int p = a + b - c;
	int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

	if (pa <= pb && pa <= pc)
		return a;
	return pb <= pc ? b : c;
}

static bool unfilter(u8 type, u8 *cur, const u8 *prev, int n)
{
	int i;

	switch (type) {
	case 0:
		return true;
	case 1:
		for (i = BPP; i < n; i++)
			cur[i] += cur[i - BPP];
		return true;
	case 2:
		for (i = 0; i < n; i++)
			cur[i] += prev[i];
		return true;
	case 3:
		for (i = 0; i < n; i++)
			cur[i] += (u8)(((i >= BPP ? cur[i - BPP] : 0) + prev[i]) >> 1);
		return true;
	case 4:
		for (i = 0; i < n; i++)
			cur[i] += (u8)paeth(i >= BPP ? cur[i - BPP] : 0, prev[i], i >= BPP ? prev[i - BPP] : 0);
		return true;
	default:
		return false;
	}
}

bool WiiPng_decodeRows(const u8 *png, size_t len, WiiPngRowFn emit, void *user)
{
	/* Filter byte + pixels. Static: pages load one at a time, on the main thread. */
	static u8 rows[2][1 + PNG_MAX_WIDTH * BPP];
	int width, height, stride, y = 0, which = 0;
	size_t pos = 8;
	z_stream z;
	bool ok = true;

	if (!WiiPng_size(png, len, &width, &height))
		return false;
	stride = width * BPP;
	memset(rows, 0, sizeof rows);
	memset(&z, 0, sizeof z);
	if (inflateInit(&z) != Z_OK)
		return false;
	z.next_out = rows[which];
	z.avail_out = (uInt)(1 + stride);

	while (ok && y < height && pos + 12 <= len) {
		u32 clen = be32(png + pos);
		const u8 *type = png + pos + 4;

		if (pos + 12 + clen > len)
			break;
		if (memcmp(type, "IEND", 4) == 0)
			break;
		if (memcmp(type, "IDAT", 4) == 0) {
			z.next_in = (Bytef *)(png + pos + 8);
			z.avail_in = clen;
			while (ok && z.avail_in > 0 && y < height) {
				int rc = inflate(&z, Z_NO_FLUSH);

				if (rc != Z_OK && rc != Z_STREAM_END) {
					ok = false;
					break;
				}
				if (z.avail_out == 0) {
					u8 *cur = rows[which];

					ok = unfilter(cur[0], cur + 1, rows[which ^ 1] + 1, stride);
					if (ok)
						emit(user, y, cur + 1, width);
					y++;
					which ^= 1;
					z.next_out = rows[which];
					z.avail_out = (uInt)(1 + stride);
				}
				if (rc == Z_STREAM_END)
					break;
			}
		}
		pos += 12 + clen;
	}
	inflateEnd(&z);
	return ok && y == height;
}
