/*
 * Copyright (c) 2011, Google Inc.
 */
#include "cache.h"
#include "streaming.h"

enum input_source {
	stream_error = -1,
	incore = 0,
	loose = 1,
	pack_non_delta = 2
};

typedef int (*open_istream_fn)(struct git_istream *,
			       struct object_info *,
			       const unsigned char *,
			       enum object_type *);
typedef int (*close_istream_fn)(struct git_istream *);
typedef ssize_t (*read_istream_fn)(struct git_istream *, char *, size_t);

struct stream_vtbl {
	close_istream_fn close;
	read_istream_fn read;
};

#define open_method_decl(name) \
	int open_istream_ ##name \
	(struct git_istream *st, struct object_info *oi, \
	 const unsigned char *sha1, \
	 enum object_type *type)

#define close_method_decl(name) \
	int close_istream_ ##name \
	(struct git_istream *st)

#define read_method_decl(name) \
	ssize_t read_istream_ ##name \
	(struct git_istream *st, char *buf, size_t sz)

/* forward declaration */
static open_method_decl(incore);
static open_method_decl(loose);
static open_method_decl(pack_non_delta);

static open_istream_fn open_istream_tbl[] = {
	open_istream_incore,
	open_istream_loose,
	open_istream_pack_non_delta,
};

struct git_istream {
	const struct stream_vtbl *vtbl;
	unsigned long size; /* inflated size of full object */
	z_stream z;
	enum { z_unused, z_used, z_done, z_error } z_state;

	union {
		struct {
			char *buf; /* from read_object() */
			unsigned long read_ptr;
		} incore;

		struct {
			int fd; /* open for reading */
			/* NEEDSWORK: what else? */
		} loose;

		struct {
			struct packed_git *pack;
			off_t pos;
		} in_pack;
	} u;
};

int close_istream(struct git_istream *st)
{
	return st->vtbl->close(st);
}

ssize_t read_istream(struct git_istream *st, char *buf, size_t sz)
{
	return st->vtbl->read(st, buf, sz);
}

static enum input_source istream_source(const unsigned char *sha1,
					enum object_type *type,
					struct object_info *oi)
{
	unsigned long size;
	int status;

	oi->sizep = &size;
	status = sha1_object_info_extended(sha1, oi);
	if (status < 0)
		return stream_error;
	*type = status;

	switch (oi->whence) {
	case OI_LOOSE:
		return loose;
	case OI_PACKED:
		if (!oi->u.packed.is_delta && big_file_threshold <= size)
			return pack_non_delta;
		/* fallthru */
	default:
		return incore;
	}
}

struct git_istream *open_istream(const unsigned char *sha1,
				 enum object_type *type,
				 unsigned long *size)
{
	struct git_istream *st;
	struct object_info oi;
	const unsigned char *real = lookup_replace_object(sha1);
	enum input_source src = istream_source(real, type, &oi);

	if (src < 0)
		return NULL;

	st = xmalloc(sizeof(*st));
	if (open_istream_tbl[src](st, &oi, real, type)) {
		if (open_istream_incore(st, &oi, real, type)) {
			free(st);
			return NULL;
		}
	}
	*size = st->size;
	return st;
}


/*****************************************************************
 *
 * Common helpers
 *
 *****************************************************************/

static void close_deflated_stream(struct git_istream *st)
{
	if (st->z_state == z_used)
		git_inflate_end(&st->z);
}


/*****************************************************************
 *
 * Loose object stream
 *
 *****************************************************************/

static open_method_decl(loose)
{
	return -1; /* for now */
}


/*****************************************************************
 *
 * Non-delta packed object stream
 *
 *****************************************************************/

static read_method_decl(pack_non_delta)
{
	size_t total_read = 0;

	switch (st->z_state) {
	case z_unused:
		memset(&st->z, 0, sizeof(st->z));
		git_inflate_init(&st->z);
		st->z_state = z_used;
		break;
	case z_done:
		return 0;
	case z_error:
		return -1;
	case z_used:
		break;
	}

	while (total_read < sz) {
		int status;
		struct pack_window *window = NULL;
		unsigned char *mapped;

		mapped = use_pack(st->u.in_pack.pack, &window,
				  st->u.in_pack.pos, &st->z.avail_in);

		st->z.next_out = (unsigned char *)buf + total_read;
		st->z.avail_out = sz - total_read;
		st->z.next_in = mapped;
		status = git_inflate(&st->z, Z_FINISH);

		st->u.in_pack.pos += st->z.next_in - mapped;
		total_read = st->z.next_out - (unsigned char *)buf;
		unuse_pack(&window);

		if (status == Z_STREAM_END) {
			git_inflate_end(&st->z);
			st->z_state = z_done;
			break;
		}
		if (status != Z_OK && status != Z_BUF_ERROR) {
			git_inflate_end(&st->z);
			st->z_state = z_error;
			return -1;
		}
	}
	return total_read;
}

static close_method_decl(pack_non_delta)
{
	close_deflated_stream(st);
	return 0;
}

static struct stream_vtbl pack_non_delta_vtbl = {
	close_istream_pack_non_delta,
	read_istream_pack_non_delta,
};

static open_method_decl(pack_non_delta)
{
	struct pack_window *window;
	enum object_type in_pack_type;

	st->u.in_pack.pack = oi->u.packed.pack;
	st->u.in_pack.pos = oi->u.packed.offset;
	window = NULL;

	in_pack_type = unpack_object_header(st->u.in_pack.pack,
					    &window,
					    &st->u.in_pack.pos,
					    &st->size);
	unuse_pack(&window);
	switch (in_pack_type) {
	default:
		return -1; /* we do not do deltas for now */
	case OBJ_COMMIT:
	case OBJ_TREE:
	case OBJ_BLOB:
	case OBJ_TAG:
		break;
	}
	st->z_state = z_unused;
	st->vtbl = &pack_non_delta_vtbl;
	return 0;
}


/*****************************************************************
 *
 * In-core stream
 *
 *****************************************************************/

static close_method_decl(incore)
{
	free(st->u.incore.buf);
	return 0;
}

static read_method_decl(incore)
{
	size_t read_size = sz;
	size_t remainder = st->size - st->u.incore.read_ptr;

	if (remainder <= read_size)
		read_size = remainder;
	if (read_size) {
		memcpy(buf, st->u.incore.buf + st->u.incore.read_ptr, read_size);
		st->u.incore.read_ptr += read_size;
	}
	return read_size;
}

static struct stream_vtbl incore_vtbl = {
	close_istream_incore,
	read_istream_incore,
};

static open_method_decl(incore)
{
	st->u.incore.buf = read_sha1_file_extended(sha1, type, &st->size, 0);
	st->u.incore.read_ptr = 0;
	st->vtbl = &incore_vtbl;

	return st->u.incore.buf ? 0 : -1;
}
