/* Decoder-only xdelta3, configured by its own config.h; first, as it sets _POSIX_SOURCE. */
#define HAVE_CONFIG_H 1
#define XD3_ENCODER 0
#define XD3_MAIN 0
#include "xdelta3/xdelta3.h"

#include "patcher.h"

#include "log.h"
#include "png_stream.h"
#include "wii_paths.h"
#include "wii_video.h"

#include <dirent.h>
#include <errno.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>
#include <malloc.h>

#define IO_CHUNK (256 << 10)
#define SRC_BLOCK (1 << 20)
#define SRC_BLOCKS 14 /* the delta copies from all over the 1.1 file: fewer blocks re-read it many times over */
#define CANVAS_BUDGET (16 << 20) /* new pages built per pass; each old page is decoded once per pass */
#define REGION_BYTES (48u << 20) /* borrowed from the top of MEM2 (51 MB free there at this point) */
#define BLOB_ALIGN 128           /* GameMaker aligns every PNG in TXTR to this */
#define PNG_LEVEL 2 /* speed over size: a page is read once per load, written once ever */
#define GUTTER 1    /* the ring kept round each item; must match tools/repack_textures.py --gutter */
#define PASS_MAX_PAGES 16

static u8 *ioBuf;
static u64 t0;
static char errLine[200]; /* the first failure, for the startup screen: later lines are its consequences */
static bool writeFailed, ranFailed;
/* Big buffers come from a region carved off MEM2's arena, not malloc: a heap that grows into MEM2
 * stays there, and the game would lose the MEM1 the texture arena normally claims. Ours grow down
 * from the top; xdelta gets a first-fit heap at the bottom (it reallocates per window). */
static u8 *regionLo, *regionHi, *bumpLo, *bumpHi;
static u32 bumpHigh;

typedef struct HeapBlock {
	u32 size; /* payload bytes */
	bool used;
	struct HeapBlock *next;
} HeapBlock;
static HeapBlock *heap; /* xdelta's blocks, address order, inside [regionLo, bumpHi) */
static u32 heapEnd;     /* bytes of the region the heap has spread over */

static bool region_take(void)
{
	u8 *hi = (u8 *)((u32)SYS_GetArena2Hi() & ~31u);

	if (SYS_GetArena2Size() < REGION_BYTES + (2u << 20))
		return false;
	regionHi = bumpHi = hi;
	regionLo = bumpLo = hi - REGION_BYTES;
	SYS_SetArena2Hi(regionLo);
	return true;
}

static void region_give_back(void)
{
	if (regionHi)
		SYS_SetArena2Hi(regionHi);
	regionLo = regionHi = bumpLo = bumpHi = NULL;
}

static void *take_top(u32 bytes)
{
	bytes = (bytes + 31) & ~31u;
	if (!bumpHi || bumpHi - bytes < bumpLo)
		return NULL;
	bumpHi -= bytes;
	memset(bumpHi, 0, bytes);
	return bumpHi;
}

static void *xd_alloc(void *opaque, size_t items, usize_t size)
{
	u32 bytes = ((u32)items * size + 31) & ~31u, hdr = (sizeof(HeapBlock) + 31) & ~31u;
	HeapBlock *b, **link = &heap;

	(void)opaque;
	if (bytes >= (1u << 20))
		logInfo("PATCH xdelta asks %u KB\n", (unsigned)(bytes >> 10));
	for (b = heap; b; link = &b->next, b = b->next) {
		if (b->used || b->size < bytes)
			continue;
		if (b->size >= bytes + hdr + 32) {
			HeapBlock *tail = (HeapBlock *)((u8 *)b + hdr + bytes);

			tail->size = b->size - bytes - hdr;
			tail->used = false;
			tail->next = b->next;
			b->size = bytes;
			b->next = tail;
		}
		b->used = true;
		return (u8 *)b + hdr;
	}
	if (!regionLo || regionLo + heapEnd + hdr + bytes > bumpHi)
		return NULL;
	b = (HeapBlock *)(regionLo + heapEnd);
	b->size = bytes;
	b->used = true;
	b->next = NULL;
	*link = b;
	heapEnd += hdr + bytes;
	bumpLo = regionLo + heapEnd;
	if (heapEnd > bumpHigh)
		bumpHigh = heapEnd;
	return (u8 *)b + hdr;
}

static void xd_free(void *opaque, void *address)
{
	u32 hdr = (sizeof(HeapBlock) + 31) & ~31u;
	HeapBlock *b = address ? (HeapBlock *)((u8 *)address - hdr) : NULL, *n;

	(void)opaque;
	if (!b)
		return;
	b->used = false;
	for (n = heap; n; n = n->next)
		while (!n->used && n->next && !n->next->used) {
			n->size += hdr + n->next->size;
			n->next = n->next->next;
		}
}

static void say(const char *fmt, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	printf("  %s\n", line);
	logInfo("PATCH %6u ms: %s\n", (unsigned)ticks_to_millisecs(gettime() - t0), line);
}

/* A failure in words a player can act on; only the first is kept. */
static void fail(const char *fmt, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	say("%s", line);
	if (!errLine[0])
		snprintf(errLine, sizeof errLine, "%s", line);
}

/* Names the one write failure a player can fix: a full card, which FatFs reports as a short write with no errno. */
static void write_failed(void)
{
	if (writeFailed)
		return;
	writeFailed = true;
	if (errno == ENOSPC || errno == 0)
		fail("%s is full (the first boot needs about 190 MB free); free some space and start again",
		     WiiPaths_media());
	else
		fail("could not write to %s (error %d)", WiiPaths_media(), errno);
}

static size_t wr(const void *p, size_t n, FILE *f)
{
	size_t r;

	errno = 0;
	r = fwrite(p, 1, n, f);

	if (r != n)
		write_failed();
	return r;
}

/* An fwrite only reached stdio's buffer; the card can still refuse it on a later flush or at the close. */
static bool close_written(FILE *f)
{
	bool ok = !ferror(f);

	errno = 0;
	if (fclose(f) != 0)
		ok = false;
	if (!ok)
		write_failed();
	return ok;
}

static bool pad_to(FILE *f, u32 align)
{
	static const u8 zeros[BLOB_ALIGN];
	long at = ftell(f);
	u32 n = at < 0 ? 0 : (align - (u32)at % align) % align;

	return at >= 0 && wr(zeros, n, f) == n;
}

static void progress(const char *what, u64 done, u64 total)
{
	printf("\x1b[20;0H  %-28s %3u%%  %4u / %4u MB   \n", what, (unsigned)(total ? done * 100 / total : 0),
	       (unsigned)(done >> 20), (unsigned)(total >> 20));
}

static bool file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static u32 rd32le(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 rd16le(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static void wr32le(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void wr16le(u8 *p, u16 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static bool copy_range(FILE *in, FILE *out, u32 off, u32 len)
{
	u32 n;

	if (fseek(in, (long)off, SEEK_SET) != 0)
		return false;
	while (len) {
		n = len < IO_CHUNK ? len : IO_CHUNK;
		if (fread(ioBuf, 1, n, in) != n || wr(ioBuf, n, out) != n)
			return false;
		len -= n;
	}
	return true;
}

/* ---- 1. the player's zip: data.win and the 1.1 music ---- */

static char zipPath[300];  /* the player's AM2R 1.1 zip, whatever it ended up being called */
static char xdeltaPath[96], planPath[96], filesDir[96], data11Path[96], data155Path[96], partPath[112];
static char zipName[128];  /* its file name, for the messages */
static char zipPrefix[96]; /* "" or "AM2R_11/": a zip of the folder instead of its contents */

static bool ends_with(const char *s, const char *tail)
{
	size_t n = strlen(s), t = strlen(tail);

	return n >= t && strcasecmp(s + n - t, tail) == 0;
}

static bool zip_entry_out(const char *name, char *out, size_t cap)
{
	size_t p = strlen(zipPrefix), n;

	if (strncmp(name, zipPrefix, p) != 0)
		return false;
	name += p;
	n = strlen(name);
	if (strcasecmp(name, "data.win") == 0)
		snprintf(out, cap, "%s", data11Path);
	else if (n > 4 && ends_with(name, ".ogg") && !strchr(name, '/'))
		snprintf(out, cap, "%s/%s", WII_GAME_DIR, name);
	else
		return false;
	return true;
}

static bool zip_inflate(FILE *zf, u32 dataOff, u32 csize, u32 usize, int method, u32 crc, const char *dst, bool show)
{
	FILE *out = fopen(dst, "wb");
	z_stream z;
	u8 *inb = ioBuf, *outb = ioBuf + IO_CHUNK / 2;
	u32 left = csize, got = 0, have = 0;
	bool ok = true;

	if (!out)
		return false;
	memset(&z, 0, sizeof z);
	if (method == 8 && inflateInit2(&z, -MAX_WBITS) != Z_OK) {
		fclose(out);
		return false;
	}
	fseek(zf, (long)dataOff, SEEK_SET);
	while (ok && (left || (method == 8 && z.avail_in))) {
		if (z.avail_in == 0 && left) {
			u32 n = left < IO_CHUNK / 2 ? left : IO_CHUNK / 2;

			if (fread(inb, 1, n, zf) != n) {
				ok = false;
				break;
			}
			left -= n;
			z.next_in = inb;
			z.avail_in = n;
		}
		if (method == 0) {
			have = crc32(have, z.next_in, z.avail_in);
			ok = wr(z.next_in, z.avail_in, out) == z.avail_in;
			got += z.avail_in;
			z.avail_in = 0;
		} else {
			int rc;

			z.next_out = outb;
			z.avail_out = IO_CHUNK / 2;
			rc = inflate(&z, Z_NO_FLUSH);
			if (rc != Z_OK && rc != Z_STREAM_END) {
				ok = false;
				break;
			}
			have = crc32(have, outb, IO_CHUNK / 2 - z.avail_out);
			ok = wr(outb, IO_CHUNK / 2 - z.avail_out, out) == IO_CHUNK / 2 - z.avail_out;
			got += IO_CHUNK / 2 - z.avail_out;
			if (rc == Z_STREAM_END)
				break;
		}
		if (show && (got & ((4u << 20) - 1)) < (u32)IO_CHUNK)
			progress("unpacking data.win", got, usize);
	}
	if (method == 8)
		inflateEnd(&z);
	if (!close_written(out))
		ok = false;
	if (ok && (got != usize || have != crc)) {
		fail("a file in %s is damaged (%s: %u of %u bytes, or a bad checksum); download the zip again",
		     zipName, dst, (unsigned)got, (unsigned)usize);
		ok = false;
	}
	return ok;
}

/* The central directory's offset and entry count, from the end record (22 bytes, behind a comment up to 64 KB). */
static bool zip_directory(FILE *zf, u32 *cdOff, u32 *cdN)
{
	u8 hdr[22];
	long size, at;

	fseek(zf, 0, SEEK_END);
	size = ftell(zf);
	for (at = size - 22; at >= 0 && at > size - 65600; at--) {
		fseek(zf, at, SEEK_SET);
		if (fread(hdr, 1, 22, zf) == 22 && rd32le(hdr) == 0x06054b50) {
			*cdN = rd16le(hdr + 10);
			*cdOff = rd32le(hdr + 16);
			return true;
		}
	}
	return false;
}

/* Where the zip keeps data.win: at the top, or one folder down (someone zipped the folder). */
static bool zip_find_data(FILE *zf, u32 cdOff, u32 cdN, char *prefix, size_t cap)
{
	u8 hdr[46];
	char name[256];
	u32 i, nlen;

	fseek(zf, (long)cdOff, SEEK_SET);
	for (i = 0; i < cdN; i++) {
		if (fread(hdr, 1, 46, zf) != 46 || rd32le(hdr) != 0x02014b50)
			return false;
		nlen = rd16le(hdr + 28);
		if (nlen >= sizeof name || fread(name, 1, nlen, zf) != nlen)
			return false;
		name[nlen] = '\0';
		fseek(zf, (long)(rd16le(hdr + 30) + rd16le(hdr + 32)), SEEK_CUR);
		if (strcasecmp(name, "data.win") == 0 ||
		    (ends_with(name, "/data.win") && strchr(name, '/') == name + nlen - 9 && nlen - 8 < cap)) {
			snprintf(prefix, cap, "%.*s", (int)(nlen - 8), name);
			return true;
		}
	}
	return false;
}

/* Any .zip beside the app or at the card's root that holds a data.win: names get mangled on the way
 * (AM2R_11.zip.zip, "AM2R_11 (1).zip"). With none, a zip beside the app stands in so its problem is named. */
static bool find_zip(void)
{
	const char *dirs[] = { WII_APP_DIR, WII_DRIVE };
	char path[300], first[300] = "", root[20];
	struct dirent *e;
	u32 cdOff, cdN;
	unsigned d;
	FILE *zf;
	DIR *dir;

	snprintf(path, sizeof path, "%s/AM2R_11.zip", WII_APP_DIR);
	if ((zf = fopen(path, "rb"))) { /* the right name wins over any other zip lying around */
		bool ok = zip_directory(zf, &cdOff, &cdN) && zip_find_data(zf, cdOff, cdN, zipPrefix, sizeof zipPrefix);

		fclose(zf);
		snprintf(zipPath, sizeof zipPath, "%s", path);
		snprintf(zipName, sizeof zipName, "AM2R_11.zip");
		if (ok)
			return true;
		snprintf(first, sizeof first, "%s", zipPath);
	}
	snprintf(root, sizeof root, "%s/", WII_DRIVE);
	for (d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
		if (!(dir = opendir(d == 0 ? dirs[0] : root)))
			continue;
		while ((e = readdir(dir))) {
			if (e->d_name[0] == '.' || !ends_with(e->d_name, ".zip"))
				continue;
			snprintf(path, sizeof path, "%s/%s", dirs[d], e->d_name);
			if (!first[0] && d == 0) /* a stranger's zip at the card's root is not worth naming */
				snprintf(first, sizeof first, "%s", path);
			if (!(zf = fopen(path, "rb")))
				continue;
			if (zip_directory(zf, &cdOff, &cdN) &&
			    zip_find_data(zf, cdOff, cdN, zipPrefix, sizeof zipPrefix)) {
				fclose(zf);
				snprintf(zipPath, sizeof zipPath, "%s", path);
				/* e belongs to dir: before closedir */
				snprintf(zipName, sizeof zipName, "%s", e->d_name);
				closedir(dir);
				return true;
			}
			fclose(zf);
		}
		closedir(dir);
	}
	if (!first[0])
		return false;
	snprintf(zipPath, sizeof zipPath, "%s", first);
	snprintf(zipName, sizeof zipName, "%s", strrchr(first, '/') + 1);
	return true;
}

static bool unzip_11(void)
{
	FILE *zf = fopen(zipPath, "rb");
	u8 hdr[46 + 512];
	u32 cdOff, cdN, i, found = 0;

	if (!zf)
		return false;
	if (!zip_directory(zf, &cdOff, &cdN)) {
		fail("%s is not a zip file", zipName);
		fclose(zf);
		return false;
	}
	if (!zip_find_data(zf, cdOff, cdN, zipPrefix, sizeof zipPrefix)) {
		fail("%s has no data.win in it; it should be the AM2R 1.1 download", zipName);
		fclose(zf);
		return false;
	}
	if (zipPrefix[0])
		say("%s keeps the game in %s", zipName, zipPrefix);
	fseek(zf, (long)cdOff, SEEK_SET);
	for (i = 0; i < cdN; i++) {
		u32 method, crc, csize, usize, nlen, xlen, clen, loc, dataOff;
		char name[256], dst[320];
		u8 lh[30];
		long next;

		if (fread(hdr, 1, 46, zf) != 46 || rd32le(hdr) != 0x02014b50)
			break;
		method = rd16le(hdr + 10);
		crc = rd32le(hdr + 16);
		csize = rd32le(hdr + 20);
		usize = rd32le(hdr + 24);
		nlen = rd16le(hdr + 28);
		xlen = rd16le(hdr + 30);
		clen = rd16le(hdr + 32);
		loc = rd32le(hdr + 42);
		if (nlen >= sizeof name || fread(name, 1, nlen, zf) != nlen)
			break;
		name[nlen] = '\0';
		next = ftell(zf) + (long)xlen + (long)clen;
		if (zip_entry_out(name, dst, sizeof dst) && (method == 0 || method == 8)) {
			fseek(zf, (long)loc, SEEK_SET);
			if (fread(lh, 1, 30, zf) != 30 || rd32le(lh) != 0x04034b50)
				break;
			dataOff = loc + 30 + rd16le(lh + 26) + rd16le(lh + 28);
			if (!zip_inflate(zf, dataOff, csize, usize, (int)method, crc, dst, usize > (8 << 20))) {
				fclose(zf);
				return false;
			}
			found++;
		}
		fseek(zf, next, SEEK_SET);
	}
	fclose(zf);
	say("unpacked %u files from %s", (unsigned)found, zipName);
	if (!file_exists(data11Path))
		fail("could not unpack data.win from %s", zipName);
	return file_exists(data11Path);
}

/* ---- 2. the community delta: 1.1 data.win -> 1.5.5 data.win ---- */

typedef struct {
	FILE *f;
	u8 *blocks[SRC_BLOCKS];
	xoff_t blkno[SRC_BLOCKS];
	u32 age[SRC_BLOCKS], tick, calls, misses;
	u64 readMs;
} SrcCache;

static int src_getblk(xd3_stream *stream, xd3_source *source, xoff_t blkno)
{
	SrcCache *c = (SrcCache *)stream->opaque;
	int i, victim = 0;
	usize_t n;
	u64 t;

	c->calls++;
	for (i = 0; i < SRC_BLOCKS; i++) {
		if (c->blkno[i] == blkno) {
			c->age[i] = ++c->tick;
			source->curblkno = blkno;
			source->curblk = c->blocks[i];
			source->onblk = (usize_t)(blkno == source->max_blkno ? source->onlastblk : SRC_BLOCK);
			return 0;
		}
		if (c->age[i] < c->age[victim])
			victim = i;
	}
	c->misses++;
	t = gettime();
	if (fseek(c->f, (long)(blkno * SRC_BLOCK), SEEK_SET) != 0)
		return XD3_INTERNAL;
	n = (usize_t)fread(c->blocks[victim], 1, SRC_BLOCK, c->f);
	c->readMs += ticks_to_millisecs(gettime() - t);
	c->blkno[victim] = blkno;
	c->age[victim] = ++c->tick;
	source->curblkno = blkno;
	source->curblk = c->blocks[victim];
	source->onblk = n;
	return 0;
}

static bool apply_delta(void)
{
	xd3_stream stream;
	xd3_config cfg;
	xd3_source src;
	SrcCache cache;
	FILE *in = fopen(xdeltaPath, "rb"), *out = fopen(data155Path, "wb");
	long srcSize, inSize;
	u64 written = 0;
	bool ok = true, eof = false;
	int i, ret;

	memset(&cache, 0, sizeof cache);
	cache.f = fopen(data11Path, "rb");
	if (!in || !out || !cache.f) {
		fail("cannot open patch/data.xdelta, or cannot write the new data.win");
		return false;
	}
	for (i = 0; i < SRC_BLOCKS; i++) {
		cache.blocks[i] = (u8 *)take_top(SRC_BLOCK);
		cache.blkno[i] = (xoff_t)-1;
		if (!cache.blocks[i])
			return false;
	}
	fseek(cache.f, 0, SEEK_END);
	srcSize = ftell(cache.f);
	fseek(in, 0, SEEK_END);
	inSize = ftell(in);
	fseek(in, 0, SEEK_SET);
	xd3_init_config(&cfg, XD3_ADLER32);
	cfg.getblk = src_getblk;
	cfg.alloc = xd_alloc;
	cfg.freef = xd_free;
	cfg.opaque = &cache;
	memset(&stream, 0, sizeof stream);
	memset(&src, 0, sizeof src);
	if (xd3_config_stream(&stream, &cfg) != 0) {
		fail("the patch could not start (xdelta config)");
		return false;
	}
	src.blksize = SRC_BLOCK;
	src.max_winsize = 8 * SRC_BLOCK;
	xd3_set_source_and_size(&stream, &src, (xoff_t)srcSize);
	while (ok) {
		if (stream.avail_in == 0 && !eof) {
			size_t n = fread(ioBuf, 1, IO_CHUNK, in);

			if (n == 0) {
				eof = true;
				xd3_set_flags(&stream, stream.flags | XD3_FLUSH);
			}
			xd3_avail_input(&stream, ioBuf, n);
		}
		ret = xd3_decode_input(&stream);
		if (ret == XD3_INPUT) {
			if (eof && stream.avail_in == 0)
				break;
			continue;
		}
		if (ret == XD3_OUTPUT) {
			ok = wr(stream.next_out, stream.avail_out, out) == stream.avail_out;
			written += stream.avail_out;
			xd3_consume_output(&stream);
			if ((written & ((8u << 20) - 1)) < (u64)stream.avail_out + 1)
				progress("applying 1.5.5 delta", (u64)(inSize - (ftell(in) - (long)stream.avail_in)),
					 (u64)inSize);
			continue;
		}
		if (ret == XD3_GOTHEADER || ret == XD3_WINSTART || ret == XD3_WINFINISH)
			continue;
		fail("%s is not AM2R 1.1: the 1.5.5 patch does not fit it (%s)", zipName, xd3_errstring(&stream));
		ok = false;
	}
	if (ok && xd3_close_stream(&stream) != 0) {
		fail("patch/data.xdelta ends early (%s); copy the whole release again", xd3_errstring(&stream));
		ok = false;
	}
	xd3_free_stream(&stream);
	fclose(cache.f);
	fclose(in);
	if (!close_written(out))
		ok = false;
	say("delta applied: %u MB written; source blocks %u asked, %u read (%u MB, %u ms); xdelta used %u KB",
	    (unsigned)(written >> 20), cache.calls, cache.misses, (unsigned)(cache.misses * (SRC_BLOCK >> 20)),
	    (unsigned)cache.readMs, (unsigned)(bumpHigh >> 10));
	return ok;
}

/* ---- 3. repack the texture pages the way tools/repack_textures.py did, from its plan ---- */

typedef struct {
	u16 w, h;
} PlanPage;

typedef struct {
	u16 page, x, y; /* destination, gutter included */
} PlanItem;

typedef struct {
	u32 count;
	PlanPage *pages;
	PlanItem *items;
} Plan;

typedef struct {
	u16 sx, sy, w, h;
	s16 page;
} Rect;

typedef struct {
	u32 off, size;
} Chunk;

typedef struct {
	u8 *canvas[PASS_MAX_PAGES];
	u32 first, n;      /* new pages in this pass */
	const Plan *plan;
	const Rect *rects;
	int oldW, oldH;
	s16 oldPage;
} Pass;

static bool read_plan(Plan *plan, const u8 *tpag, u32 tpagSize)
{
	FILE *f = fopen(planPath, "rb");
	u8 hdr[20];
	u32 pages, i;
	bool ok = true;

	if (!f)
		return false;
	if (fread(hdr, 1, 20, f) != 20 || memcmp(hdr, "AM2RPLAN", 8) != 0) {
		fail("patch/plan.bin is damaged; copy the whole release again");
		fclose(f);
		return false;
	}
	if (rd32le(hdr + 8) != (u32)crc32(0, tpag, tpagSize)) {
		fail("patch/plan.bin belongs to a different release; copy the whole release again");
		fclose(f);
		return false;
	}
	pages = rd32le(hdr + 12);
	plan->count = rd32le(hdr + 16);
	if (!pages || pages > 4096 || plan->count != rd32le(tpag)) {
		fail("patch/plan.bin does not match the patched data.win; copy the whole release again");
		fclose(f);
		return false;
	}
	plan->pages = (PlanPage *)take_top(pages * sizeof(PlanPage));
	plan->items = (PlanItem *)take_top(plan->count * sizeof(PlanItem));
	if (!plan->pages || !plan->items) {
		fclose(f);
		return false;
	}
	/* a copy cut short still passes the TPAG check above; its records would become canvas addresses */
	for (i = 0; ok && i < pages; i++) {
		ok = fread(hdr, 1, 4, f) == 4;
		plan->pages[i].w = rd16le(hdr);
		plan->pages[i].h = rd16le(hdr + 2);
		ok = ok && plan->pages[i].w && plan->pages[i].w <= 1024 && plan->pages[i].h && plan->pages[i].h <= 1024;
	}
	for (i = 0; ok && i < plan->count; i++) {
		ok = fread(hdr, 1, 6, f) == 6;
		plan->items[i].page = rd16le(hdr);
		plan->items[i].x = rd16le(hdr + 2);
		plan->items[i].y = rd16le(hdr + 4);
		ok = ok && plan->items[i].page < pages;
	}
	fclose(f);
	if (!ok) {
		fail("patch/plan.bin is damaged; copy the whole release again");
		return false;
	}
	say("plan: %u items onto %u pages", (unsigned)plan->count, (unsigned)pages);
	return true;
}

/* Row y of old page P lands in every canvas of the pass that holds one of its items; the ring
 * of GUTTER pixels round each item comes along, edge pixels repeated where the page ends. */
static void pass_row(void *user, int y, const u8 *rgba, int width)
{
	Pass *p = (Pass *)user;
	u32 i;

	for (i = 0; i < p->plan->count; i++) {
		const Rect *r = &p->rects[i];
		const PlanItem *d = &p->plan->items[i];
		int top, bottom, dy, dx, x0, x1, cw;
		u8 *canvas;

		if (r->page != p->oldPage || d->page < p->first || d->page >= p->first + p->n)
			continue;
		top = r->sy - GUTTER;
		bottom = r->sy + r->h + GUTTER; /* exclusive */
		if (y < (top < 0 ? 0 : top) || y >= (bottom > p->oldH ? p->oldH : bottom))
			continue;
		canvas = p->canvas[d->page - p->first];
		cw = p->plan->pages[d->page].w;
		x0 = r->sx - GUTTER;
		x1 = r->sx + r->w + GUTTER;
		/* rows this source row fills: itself, plus the ring row it stands in for at a page edge */
		for (dy = (y == 0 && top < 0) ? -1 : 0; dy <= ((y == p->oldH - 1 && bottom > p->oldH) ? 1 : 0); dy++) {
			u8 *dst = canvas + ((size_t)(d->y - GUTTER + (y - top) + dy) * cw + (d->x - GUTTER)) * 4;

			for (dx = x0; dx < x1; dx++) {
				int sx = dx < 0 ? 0 : dx >= width ? width - 1 : dx;

				memcpy(dst + (dx - x0) * 4, rgba + sx * 4, 4);
			}
		}
	}
}

static bool png_write_chunk(FILE *f, const char *kind, const u8 *body, u32 len)
{
	u8 head[8];
	u32 crc = crc32(0, (const u8 *)kind, 4);

	head[0] = (u8)(len >> 24);
	head[1] = (u8)(len >> 16);
	head[2] = (u8)(len >> 8);
	head[3] = (u8)len;
	memcpy(head + 4, kind, 4);
	if (len)
		crc = crc32(crc, body, len);
	{
		u8 tail[4] = { (u8)(crc >> 24), (u8)(crc >> 16), (u8)(crc >> 8), (u8)crc };

		return wr(head, 8, f) == 8 && (!len || wr(body, len, f) == len) && wr(tail, 4, f) == 4;
	}
}

/* Unfiltered rows, as GameMaker writes them; several IDATs so no page-sized buffer is needed. */
static bool png_write(FILE *f, const u8 *canvas, int w, int h)
{
	static const u8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	u8 ihdr[13] = { (u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w,
			(u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h,
			8, 6, 0, 0, 0 };
	u8 *row = ioBuf, *out = ioBuf + (w * 4 + 1);
	u32 outCap = IO_CHUNK - (u32)(w * 4 + 1);
	z_stream z;
	int y, rc;
	bool ok;

	if (wr(sig, 8, f) != 8 || !png_write_chunk(f, "IHDR", ihdr, 13))
		return false;
	memset(&z, 0, sizeof z);
	if (deflateInit(&z, PNG_LEVEL) != Z_OK)
		return false;
	ok = true;
	for (y = 0; ok && y <= h; y++) {
		int flush = y == h ? Z_FINISH : Z_NO_FLUSH;

		if (y < h) {
			row[0] = 0;
			memcpy(row + 1, canvas + (size_t)y * w * 4, (size_t)w * 4);
			z.next_in = row;
			z.avail_in = (uInt)(w * 4 + 1);
		}
		do {
			z.next_out = out;
			z.avail_out = outCap;
			rc = deflate(&z, flush);
			if (rc == Z_STREAM_ERROR) {
				ok = false;
				break;
			}
			if (outCap - z.avail_out)
				ok = png_write_chunk(f, "IDAT", out, outCap - z.avail_out);
		} while (ok && z.avail_out == 0);
	}
	deflateEnd(&z);
	return ok && png_write_chunk(f, "IEND", NULL, 0);
}

static bool find_chunks(FILE *f, Chunk *tpag, Chunk *txtr, Chunk *audo, u32 *total)
{
	u8 h[8];
	u32 off = 8, size;

	fseek(f, 0, SEEK_END);
	*total = (u32)ftell(f);
	memset(tpag, 0, sizeof *tpag);
	memset(txtr, 0, sizeof *txtr);
	memset(audo, 0, sizeof *audo);
	while (off + 8 <= *total) {
		fseek(f, (long)off, SEEK_SET);
		if (fread(h, 1, 8, f) != 8)
			return false;
		size = rd32le(h + 4);
		if (memcmp(h, "TPAG", 4) == 0)
			*tpag = (Chunk){ off + 8, size };
		else if (memcmp(h, "TXTR", 4) == 0)
			*txtr = (Chunk){ off + 8, size };
		else if (memcmp(h, "AUDO", 4) == 0)
			*audo = (Chunk){ off + 8, size };
		off += 8 + size;
	}
	return tpag->size && txtr->size && audo->size && audo->off + audo->size == *total;
}

static bool repack(void)
{
	FILE *in = fopen(data155Path, "rb"), *out = NULL;
	Chunk tpag, txtr, audo;
	u32 total, n, i, oldN, newN, flag = 0, entryAt, txtrBodyAt, outTxtrSize, delta, first;
	u8 *tpagBuf = NULL, *txtrHead = NULL, *blob = NULL, *mark;
	u32 *oldOff = NULL, *newOff = NULL, blobMax = 0;
	Rect *rects = NULL;
	const PlanPage *pg;
	Plan plan;
	Pass pass;
	bool ok = false;

	memset(&plan, 0, sizeof plan);
	memset(&pass, 0, sizeof pass);
	if (!in || !find_chunks(in, &tpag, &txtr, &audo, &total)) {
		fail("the patched data.win is not the expected AM2R 1.5.5; copy the whole release again");
		goto done;
	}
	tpagBuf = (u8 *)take_top(tpag.size);
	fseek(in, (long)tpag.off, SEEK_SET);
	if (!tpagBuf || fread(tpagBuf, 1, tpag.size, in) != tpag.size || !read_plan(&plan, tpagBuf, tpag.size))
		goto done;
	n = plan.count;
	rects = (Rect *)take_top(n * sizeof(Rect));
	if (!rects)
		goto done;
	for (i = 0; i < n; i++) {
		u8 *e = tpagBuf + (rd32le(tpagBuf + 4 + 4 * i) - tpag.off);

		rects[i].sx = rd16le(e);
		rects[i].sy = rd16le(e + 2);
		rects[i].w = rd16le(e + 4);
		rects[i].h = rd16le(e + 6);
		rects[i].page = (s16)rd16le(e + 20);
		pg = &plan.pages[plan.items[i].page];
		if (plan.items[i].x < GUTTER || plan.items[i].y < GUTTER ||
		    plan.items[i].x + rects[i].w + GUTTER > pg->w || plan.items[i].y + rects[i].h + GUTTER > pg->h) {
			fail("patch/plan.bin is damaged (item %u); copy the whole release again", (unsigned)i);
			goto done;
		}
		wr16le(e, plan.items[i].x);
		wr16le(e + 2, plan.items[i].y);
		wr16le(e + 20, plan.items[i].page);
	}
	/* old TXTR: pointer list, then (flag, pngOffset) entries */
	fseek(in, (long)txtr.off, SEEK_SET);
	fread(ioBuf, 1, 4, in);
	oldN = rd32le(ioBuf);
	oldOff = (u32 *)take_top((oldN + 1) * 4);
	for (i = 0; i < oldN; i++) {
		u32 e;

		fseek(in, (long)(txtr.off + 4 + 4 * i), SEEK_SET);
		fread(ioBuf, 1, 4, in);
		e = rd32le(ioBuf);
		fseek(in, (long)e, SEEK_SET);
		fread(ioBuf, 1, 8, in);
		flag = rd32le(ioBuf);
		oldOff[i] = rd32le(ioBuf + 4);
	}
	oldOff[oldN] = txtr.off + txtr.size;
	for (i = 0; i < oldN; i++)
		if (oldOff[i + 1] - oldOff[i] > blobMax)
			blobMax = oldOff[i + 1] - oldOff[i];
	blob = (u8 *)take_top(blobMax);
	newN = 0;
	for (i = 0; i < n; i++)
		if ((u32)plan.items[i].page + 1 > newN)
			newN = (u32)plan.items[i].page + 1;
	newOff = (u32 *)take_top(newN * 4);
	if (!blob || !newOff)
		goto done;

	out = fopen(partPath, "wb");
	if (!out)
		goto done;
	/* head, with the patched TPAG chunk substituted */
	{
		u32 off = 0, headLen = txtr.off - 8;

		while (off < headLen) {
			u32 len = headLen - off < IO_CHUNK ? headLen - off : IO_CHUNK, k;

			fseek(in, (long)off, SEEK_SET);
			if (fread(ioBuf, 1, len, in) != len)
				goto done;
			for (k = 0; k < len; k++)
				if (off + k >= tpag.off && off + k < tpag.off + tpag.size)
					ioBuf[k] = tpagBuf[off + k - tpag.off];
			if (wr(ioBuf, len, out) != len)
				goto done;
			off += len;
		}
	}
	/* TXTR: count, pointers, entry placeholders; the entries are filled in at the end */
	txtrBodyAt = txtr.off;
	txtrHead = (u8 *)take_top(4 + 12 * newN);
	wr32le(txtrHead, newN);
	entryAt = 4 + 4 * newN;
	for (i = 0; i < newN; i++)
		wr32le(txtrHead + 4 + 4 * i, txtrBodyAt + entryAt + 8 * i);
	wr("TXTR\0\0\0\0", 8, out);
	wr(txtrHead, 4 + 12 * newN, out);

	pass.plan = &plan;
	pass.rects = rects;
	for (first = 0; first < newN; first += pass.n) {
		u32 bytes = 0, p, o;

		pass.first = first;
		pass.n = 0;
		mark = bumpHi;
		while (first + pass.n < newN && pass.n < PASS_MAX_PAGES) {
			u32 b = (u32)plan.pages[first + pass.n].w * plan.pages[first + pass.n].h * 4;

			if (pass.n && bytes + b > CANVAS_BUDGET)
				break;
			pass.canvas[pass.n] = (u8 *)take_top(b);
			if (!pass.canvas[pass.n]) {
				fail("out of memory while rebuilding texture page %u", (unsigned)(first + pass.n));
				goto done;
			}
			bytes += b;
			pass.n++;
		}
		for (o = 0; o < oldN; o++) {
			bool needed = false;

			for (i = 0; i < n && !needed; i++)
				needed = rects[i].page == (s16)o &&
					 plan.items[i].page >= first && plan.items[i].page < first + pass.n;
			if (!needed)
				continue;
			fseek(in, (long)oldOff[o], SEEK_SET);
			if (fread(blob, 1, oldOff[o + 1] - oldOff[o], in) != oldOff[o + 1] - oldOff[o] ||
			    !WiiPng_size(blob, oldOff[o + 1] - oldOff[o], &pass.oldW, &pass.oldH)) {
				fail("texture page %u of the new data.win could not be read", (unsigned)o);
				goto done;
			}
			pass.oldPage = (s16)o;
			if (!WiiPng_decodeRows(blob, oldOff[o + 1] - oldOff[o], pass_row, &pass)) {
				fail("texture page %u of the new data.win could not be decoded", (unsigned)o);
				goto done;
			}
		}
		for (p = 0; p < pass.n; p++) {
			long at;

			if (!pad_to(out, BLOB_ALIGN))
				goto done;
			at = ftell(out);
			newOff[first + p] = (u32)at;
			if (!png_write(out, pass.canvas[p], plan.pages[first + p].w, plan.pages[first + p].h))
				goto done;
			pass.canvas[p] = NULL;
			progress("repacking textures", first + p + 1, newN);
		}
		bumpHi = mark; /* the pass's canvases, LIFO */
	}
	if (!pad_to(out, 16))
		goto done;
	outTxtrSize = (u32)ftell(out) - txtr.off;
	/* AUDO: the pointer list shifts by the TXTR growth, the sound data is copied as is */
	delta = outTxtrSize - txtr.size;
	wr("AUDO", 4, out);
	wr32le(ioBuf, audo.size);
	wr(ioBuf, 4, out);
	fseek(in, (long)audo.off, SEEK_SET);
	fread(ioBuf, 1, 4, in);
	{
		u32 an = rd32le(ioBuf), k;

		wr(ioBuf, 4, out);
		for (k = 0; k < an; k++) {
			fread(ioBuf, 1, 4, in);
			wr32le(ioBuf, rd32le(ioBuf) + delta);
			wr(ioBuf, 4, out);
		}
		if (!copy_range(in, out, audo.off + 4 + 4 * an, audo.size - 4 - 4 * an))
			goto done;
	}
	/* back-patch: TXTR size, its entries, FORM size */
	fseek(out, (long)txtr.off - 4, SEEK_SET);
	wr32le(ioBuf, outTxtrSize);
	wr(ioBuf, 4, out);
	fseek(out, (long)(txtrBodyAt + entryAt), SEEK_SET);
	for (i = 0; i < newN; i++) {
		wr32le(ioBuf, flag);
		wr32le(ioBuf + 4, newOff[i]);
		wr(ioBuf, 8, out);
	}
	fseek(out, 0, SEEK_END);
	total = (u32)ftell(out);
	fseek(out, 4, SEEK_SET);
	wr32le(ioBuf, total - 8);
	wr(ioBuf, 4, out);
	ok = true;
	say("repacked: %u pages -> %u, %u MB", (unsigned)oldN, (unsigned)newN, (unsigned)(total >> 20));
done:
	if (out && !close_written(out))
		ok = false;
	if (in)
		fclose(in);
	return ok;
}

/* ---- 4. the loose 1.5.5 files (lang/, mods/, new music) ---- */

static bool copy_tree(const char *from, const char *to)
{
	DIR *d = opendir(from);
	struct dirent *e;
	char a[512], b[512];
	struct stat st;
	bool ok = true;

	if (!d)
		return false;
	mkdir(to, 0777);
	while (ok && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(a, sizeof a, "%s/%s", from, e->d_name);
		snprintf(b, sizeof b, "%s/%s", to, e->d_name);
		if (stat(a, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			ok = copy_tree(a, b);
		} else {
			FILE *fi = fopen(a, "rb"), *fo = fopen(b, "wb");

			ok = fi && fo && copy_range(fi, fo, 0, (u32)st.st_size);
			if (fi)
				fclose(fi);
			if (fo && !close_written(fo))
				ok = false;
		}
	}
	closedir(d);
	return ok;
}

bool WiiPatcher_run(void)
{
	bool ok;

	if (file_exists(WII_DATA_WIN))
		return true;
	snprintf(xdeltaPath, sizeof xdeltaPath, "%s/patch/data.xdelta", WII_APP_DIR);
	snprintf(planPath, sizeof planPath, "%s/patch/plan.bin", WII_APP_DIR);
	snprintf(filesDir, sizeof filesDir, "%s/patch/files", WII_APP_DIR);
	snprintf(data11Path, sizeof data11Path, "%s/data11.win", WII_GAME_DIR);
	snprintf(data155Path, sizeof data155Path, "%s/data155.win", WII_GAME_DIR);
	snprintf(partPath, sizeof partPath, "%s.part", WII_DATA_WIN);
	if (!file_exists(xdeltaPath) || !file_exists(planPath) || !find_zip())
		return false;
	if (!region_take()) {
		logWarn("PATCH: not enough free MEM2 for the patcher (%u MB)\n", (unsigned)(SYS_GetArena2Size() >> 20));
		return false;
	}
	ioBuf = (u8 *)take_top(IO_CHUNK);
	if (!ioBuf || !WiiVideo_console()) {
		region_give_back();
		return false;
	}
	t0 = gettime();
	logInfo("PATCH heap before: arena=%d mem1_free=%u mem2_free=%u\n", mallinfo().arena,
		(unsigned)SYS_GetArena1Size(), (unsigned)SYS_GetArena2Size());
	printf("\n\n  AM2R GX -- first boot: building AM2R 1.5.5 from %s\n", zipName);
	printf("  This happens once and takes a few minutes. Do not turn the Wii off.\n\n");
	ok = file_exists(filesDir);
	if (!ok)
		fail("patch/files is missing; copy the whole release again");
	if (ok)
		ok = unzip_11();
	if (ok)
		ok = apply_delta();
	remove(data11Path);
	bumpLo = regionLo; /* the delta's buffers are done with; the repack starts afresh */
	bumpHi = regionHi;
	heap = NULL;
	heapEnd = 0;
	ioBuf = (u8 *)take_top(IO_CHUNK);
	if (ok)
		ok = repack();
	remove(data155Path);
	if (ok) {
		ok = copy_tree(filesDir, WII_GAME_DIR);
		if (!ok)
			fail("could not copy patch/files into %s", WII_GAME_DIR);
	}
	/* data.win appears last: a boot cut short anywhere before this starts over instead of playing
	 * a half-built game */
	if (ok && rename(partPath, WII_DATA_WIN) != 0) {
		fail("could not finish %s (error %d)", WII_DATA_WIN, errno);
		ok = false;
	}
	logInfo("PATCH heap after: arena=%d mem1_free=%u\n", mallinfo().arena, (unsigned)SYS_GetArena1Size());
	if (ok)
		say("done");
	else
		say("FAILED - see %s/profile.txt", WII_APP_DIR);
	if (!ok)
		remove(partPath);
	ioBuf = NULL;
	region_give_back();
	ranFailed = !ok;
	return ok;
}

const char *WiiPatcher_error(void)
{
	static char msg[320];

	if (!ranFailed)
		return NULL;
	snprintf(msg, sizeof msg, "The first boot could not build AM2R 1.5.5: %s.",
		 errLine[0] ? errLine : "see the log");
	return msg;
}
