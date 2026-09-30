#include "bin_files.h"

#include "utils.h"

#include <stdint.h>
#include <string.h>

/* AM2R's crypt script decrypts a save in place a byte at a time (read, seek back, write, seek on), and every
 * one of those went to the card: 0.8 s per pass in Dolphin, two passes per save read or written. */
typedef struct {
	void *file; /* the overlay's own handle, kept open for the write-back */
	int32_t mode;
	uint8_t *data;
	int32_t size, cap, pos, origSize;
	int32_t lo, hi; /* the bytes written, to put back */
	bool truncate;
} BinFile;

static FileSystemVtable wrapped;
static const FileSystemVtable *inner;

static void *bin_open(FileSystem *fs, const char *path, int32_t mode)
{
	void *file = inner->binaryOpen(fs, path, mode);
	BinFile *b;
	int32_t size;

	if (!file)
		return NULL;
	size = mode == GML_FILE_BIN_WRITE ? 0 : inner->binarySize(fs, file);
	if (size < 0)
		size = 0;
	b = (BinFile *)safeCalloc(1, sizeof *b);
	b->file = file;
	b->mode = mode;
	b->cap = size > 256 ? size : 256;
	b->data = (uint8_t *)safeMalloc((size_t)b->cap);
	if (size > 0 && inner->binarySeek(fs, file, 0))
		b->size = inner->binaryRead(fs, file, b->data, size);
	if (b->size < 0)
		b->size = 0;
	b->origSize = b->size;
	b->lo = INT32_MAX;
	return b;
}

static void bin_close(FileSystem *fs, void *handle)
{
	BinFile *b = (BinFile *)handle;

	if (!b)
		return;
	if (b->truncate || b->size < b->origSize) {
		inner->binaryRewrite(fs, b->file);
		inner->binaryWrite(fs, b->file, b->data, b->size);
	} else if (b->lo < b->hi && inner->binarySeek(fs, b->file, b->lo)) {
		inner->binaryWrite(fs, b->file, b->data + b->lo, b->hi - b->lo);
	}
	inner->binaryClose(fs, b->file);
	free(b->data);
	free(b);
}

static int32_t bin_read(FileSystem *fs, void *handle, void *dst, int32_t n)
{
	BinFile *b = (BinFile *)handle;
	int32_t left;

	(void)fs;
	if (!b || n <= 0 || b->pos >= b->size)
		return 0;
	left = b->size - b->pos;
	if (n > left)
		n = left;
	memcpy(dst, b->data + b->pos, (size_t)n);
	b->pos += n;
	return n;
}

static int32_t bin_write(FileSystem *fs, void *handle, const void *src, int32_t n)
{
	BinFile *b = (BinFile *)handle;
	int32_t end;

	(void)fs;
	if (!b || n <= 0 || b->mode == GML_FILE_BIN_READ)
		return 0;
	end = b->pos + n;
	if (end > b->cap) {
		while (b->cap < end)
			b->cap *= 2;
		b->data = (uint8_t *)safeRealloc(b->data, (size_t)b->cap);
	}
	/* A write past the end leaves zeros behind it, as on the card. */
	if (b->pos > b->size) {
		memset(b->data + b->size, 0, (size_t)(b->pos - b->size));
		if (b->size < b->lo)
			b->lo = b->size;
	}
	memcpy(b->data + b->pos, src, (size_t)n);
	if (b->pos < b->lo)
		b->lo = b->pos;
	if (end > b->hi)
		b->hi = end;
	b->pos = end;
	if (end > b->size)
		b->size = end;
	return n;
}

static int32_t bin_tell(FileSystem *fs, void *handle)
{
	(void)fs;
	return handle ? ((BinFile *)handle)->pos : 0;
}

static bool bin_seek(FileSystem *fs, void *handle, int32_t pos)
{
	(void)fs;
	if (!handle || pos < 0)
		return false;
	((BinFile *)handle)->pos = pos;
	return true;
}

static int32_t bin_size(FileSystem *fs, void *handle)
{
	(void)fs;
	return handle ? ((BinFile *)handle)->size : 0;
}

static void bin_rewrite(FileSystem *fs, void *handle)
{
	BinFile *b = (BinFile *)handle;

	(void)fs;
	if (!b)
		return;
	b->size = 0;
	b->pos = 0;
	b->truncate = true;
}

void WiiBinFiles_install(FileSystem *fs)
{
	if (!fs || fs->vtable == &wrapped)
		return;
	inner = fs->vtable;
	wrapped = *inner;
	wrapped.binaryOpen = bin_open;
	wrapped.binaryClose = bin_close;
	wrapped.binaryRead = bin_read;
	wrapped.binaryWrite = bin_write;
	wrapped.binaryTell = bin_tell;
	wrapped.binarySeek = bin_seek;
	wrapped.binarySize = bin_size;
	wrapped.binaryRewrite = bin_rewrite;
	fs->vtable = &wrapped;
}
