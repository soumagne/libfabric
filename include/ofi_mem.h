/*
 * Copyright (c) 2015-2016 Intel Corporation, Inc.  All rights reserved.
 * Copyright (c) 2018 Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _OFI_MEM_H_
#define _OFI_MEM_H_

#include <config.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <ofi_list.h>
#include <ofi_osd.h>


#ifdef INCLUDE_VALGRIND
#   include <valgrind/memcheck.h>
#   ifndef VALGRIND_MAKE_MEM_DEFINED
#      warning "Valgrind requested, but VALGRIND_MAKE_MEM_DEFINED undefined"
#   endif
#endif

#ifndef VALGRIND_MAKE_MEM_DEFINED
#   define VALGRIND_MAKE_MEM_DEFINED(addr, len)
#endif

/* We implement memdup to avoid external library dependency */
static inline void *mem_dup(const void *src, size_t size)
{
	void *dest = malloc(size);

	if (dest)
		memcpy(dest, src, size);
	return dest;
}

static inline int ofi_str_dup(const char *src, char **dst)
{
	if (src) {
		*dst = strdup(src);
		if (!*dst)
			return -FI_ENOMEM;
	} else {
		*dst = NULL;
	}
	return 0;
}

/*
 * Buffer pool (free stack) template
 */
#define FREESTACK_EMPTY	NULL

#define freestack_get_next(user_buf)	((char *)user_buf - sizeof(void *))
#define freestack_get_user_buf(entry)	((char *)entry + sizeof(void *))

#if ENABLE_DEBUG
#define freestack_init_next(entry)	*((void **)entry) = NULL
#define freestack_check_next(entry)	assert(*((void **)entry) == NULL)
#else
#define freestack_init_next(entry)
#define freestack_check_next(entry)
#endif

#define FREESTACK_HEADER 					\
	size_t		size;					\
	void		*next;					\

#define freestack_isempty(fs)	((fs)->next == FREESTACK_EMPTY)
#define freestack_push(fs, p)					\
do {								\
	freestack_check_next(freestack_get_next(p));		\
	*(void **) (freestack_get_next(p)) = (fs)->next;	\
	(fs)->next = (freestack_get_next(p));			\
} while (0)
#define freestack_pop(fs) freestack_pop_impl(fs, (fs)->next)

static inline void* freestack_pop_impl(void *fs, void *fs_next)
{
	struct {
		FREESTACK_HEADER
	} *freestack = fs;
	assert(!freestack_isempty(freestack));
	freestack->next = *((void **)fs_next);
	freestack_init_next(fs_next);
	return freestack_get_user_buf(fs_next);
}

#define DECLARE_FREESTACK(entrytype, name)			\
struct name ## _entry {						\
	void		*next;					\
	entrytype	buf;					\
};								\
struct name {							\
	FREESTACK_HEADER					\
	struct name ## _entry	entry[];			\
};								\
								\
typedef void (*name ## _entry_init_func)(entrytype *buf,	\
					 void *arg);		\
								\
static inline void						\
name ## _init(struct name *fs, size_t size,			\
	      name ## _entry_init_func init, void *arg)		\
{								\
	ssize_t i;						\
	assert(size == roundup_power_of_two(size));		\
	assert(sizeof(fs->entry[0].buf) >= sizeof(void *));	\
	fs->size = size;					\
	fs->next = FREESTACK_EMPTY;				\
	for (i = size - 1; i >= 0; i--) {			\
		if (init)					\
			init(&fs->entry[i].buf, arg);		\
		freestack_push(fs, &fs->entry[i].buf);		\
	}							\
}								\
								\
static inline struct name *					\
name ## _create(size_t size, name ## _entry_init_func init,	\
		void *arg)					\
{								\
	struct name *fs;					\
	fs = calloc(1, sizeof(*fs) +				\
		       sizeof(struct name ## _entry) *		\
		       (roundup_power_of_two(size)));		\
	if (fs)							\
		name ##_init(fs, roundup_power_of_two(size),	\
			     init, arg);			\
	return fs;						\
}								\
								\
static inline int name ## _index(struct name *fs,		\
				 entrytype *entry)		\
{								\
	return (int)((struct name ## _entry *)			\
			(freestack_get_next(entry))		\
			- (struct name ## _entry *)fs->entry);	\
}								\
								\
static inline void name ## _free(struct name *fs)		\
{								\
	free(fs);						\
}

/*
 * Buffer pool (free stack) template for shared memory regions
 */
#define SMR_FREESTACK_EMPTY	NULL

#define SMR_FREESTACK_HEADER 					\
	void		*base_addr;				\
	size_t		size;					\
	void		*next;					\

#define smr_freestack_isempty(fs)	((fs)->next == SMR_FREESTACK_EMPTY)
#define smr_freestack_push(fs, local_p)				\
do {								\
	void *p = (char **) fs->base_addr +			\
	    ((char **) freestack_get_next(local_p) -		\
		(char **) fs);					\
	*(void **) freestack_get_next(local_p) = (fs)->next;	\
	(fs)->next = p;						\
} while (0)
#define smr_freestack_pop(fs) smr_freestack_pop_impl(fs, fs->next)

static inline void* smr_freestack_pop_impl(void *fs, void *next)
{
	void *local;

	struct {
		SMR_FREESTACK_HEADER
	} *freestack = fs;
	assert(next != NULL);

	local = (char **) fs + ((char **) next -
		(char **) freestack->base_addr);
	next = *((void **) local);
	return freestack_get_user_buf(local);
}

#define DECLARE_SMR_FREESTACK(entrytype, name)			\
struct name ## _entry {						\
	void		*next;					\
	entrytype	buf;					\
};								\
struct name {							\
	SMR_FREESTACK_HEADER					\
	struct name ## _entry	entry[];			\
};								\
								\
static inline void name ## _init(struct name *fs, size_t size)	\
{								\
	ssize_t i;						\
	assert(size == roundup_power_of_two(size));		\
	assert(sizeof(fs->entry[0].buf) >= sizeof(void *));	\
	fs->size = size;					\
	fs->next = SMR_FREESTACK_EMPTY;				\
	fs->base_addr = fs;					\
	for (i = size - 1; i >= 0; i--)				\
		smr_freestack_push(fs, &fs->entry[i].buf);	\
}								\
								\
static inline struct name * name ## _create(size_t size)	\
{								\
	struct name *fs;					\
	fs = calloc(1, sizeof(*fs) + sizeof(entrytype) *	\
		    (roundup_power_of_two(size)));		\
	if (fs)							\
		name ##_init(fs, roundup_power_of_two(size));	\
	return fs;						\
}								\
								\
static inline int name ## _index(struct name *fs,		\
		entrytype *entry)				\
{								\
	return (int)((struct name ## _entry *)			\
			(freestack_get_next(entry))		\
			- (struct name ## _entry *)fs->entry);	\
}								\
								\
static inline void name ## _free(struct name *fs)		\
{								\
	free(fs);						\
}


/*
 * Buffer Pool
 */

#define UTIL_BUF_POOL_REGION_CHUNK_CNT	16

struct util_buf_pool;
typedef int (*util_buf_region_alloc_hndlr) (void *pool_ctx, void *addr, size_t len,
					    void **context);
typedef void (*util_buf_region_free_hndlr) (void *pool_ctx, void *context);
typedef void (*util_buf_region_init_func) (void *pool_ctx, void *buf);

struct util_buf_attr {
	size_t 				size;
	size_t 				alignment;
	size_t	 			max_cnt;
	size_t 				chunk_cnt;
	util_buf_region_alloc_hndlr 	alloc_hndlr;
	util_buf_region_free_hndlr 	free_hndlr;
	util_buf_region_init_func 	init;
	void 				*ctx;
	uint8_t				track_used;
	uint8_t				is_mmap_region;
	uint8_t				use_ftr;
};

struct util_buf_pool {
	size_t 			entry_sz;
	size_t 			num_allocated;
	struct slist		buf_list;
	struct slist		region_list;
	struct util_buf_region	**regions_table;
	size_t			regions_cnt;
	struct util_buf_attr	attr;
};

struct util_buf_region {
	struct slist_entry entry;
	char *mem_region;
	size_t size;
	void *context;
#if ENABLE_DEBUG
	size_t num_used;
#endif
};

struct util_buf_footer {
	struct util_buf_region *region;
	size_t index;
};

union util_buf {
	struct slist_entry entry;
	uint8_t data[0];
};

int util_buf_pool_create_attr(struct util_buf_attr *attr,
			      struct util_buf_pool **buf_pool);

/* create buffer pool with alloc/free handlers */
int util_buf_pool_create_ex(struct util_buf_pool **buf_pool,
			    size_t size, size_t alignment,
			    size_t max_cnt, size_t chunk_cnt,
			    util_buf_region_alloc_hndlr alloc_hndlr,
			    util_buf_region_free_hndlr free_hndlr,
			    void *pool_ctx);

/* create buffer pool */
static inline int util_buf_pool_create(struct util_buf_pool **pool,
				       size_t size, size_t alignment,
				       size_t max_cnt, size_t chunk_cnt)
{
	return util_buf_pool_create_ex(pool, size, alignment,
				       max_cnt, chunk_cnt,
				       NULL, NULL, NULL);
}

static inline int util_buf_avail(struct util_buf_pool *pool)
{
	return !slist_empty(&pool->buf_list);
}

int util_buf_grow(struct util_buf_pool *pool);

#if ENABLE_DEBUG

void *util_buf_get(struct util_buf_pool *pool);
void util_buf_release(struct util_buf_pool *pool, void *buf);
size_t util_get_buf_index(struct util_buf_pool *pool, void *buf);
void *util_buf_get_by_index(struct util_buf_pool *pool, size_t index);

#else

static inline void *util_buf_get(struct util_buf_pool *pool)
{
	struct slist_entry *entry;
	entry = slist_remove_head(&pool->buf_list);
	return entry;
}

static inline void util_buf_release(struct util_buf_pool *pool, void *buf)
{
	union util_buf *util_buf = buf;
	slist_insert_head(&util_buf->entry, &pool->buf_list);
}

static inline size_t util_get_buf_index(struct util_buf_pool *pool, void *buf)
{
	return ((struct util_buf_footer *)((char *)buf + pool->attr.size))->index;
}
static inline void *util_buf_get_by_index(struct util_buf_pool *pool, size_t index)
{
	return (union util_buf *)(pool->regions_table[
		(size_t)(index / pool->attr.chunk_cnt)]->mem_region +
		(index % pool->attr.chunk_cnt) * pool->entry_sz);
}

#endif

static inline void *util_buf_get_ex(struct util_buf_pool *pool, void **context)
{
	union util_buf *buf;
	struct util_buf_footer *buf_ftr;

	buf = util_buf_get(pool);
	buf_ftr = (struct util_buf_footer *) ((char *) buf + pool->attr.size);
	assert(context);
	*context = buf_ftr->region->context;
	return buf;
}

static inline void *util_buf_alloc(struct util_buf_pool *pool)
{
	if (OFI_UNLIKELY(!util_buf_avail(pool))) {
		if (util_buf_grow(pool))
			return NULL;
	}
	return util_buf_get(pool);
}

static inline void *util_buf_alloc_ex(struct util_buf_pool *pool, void **context)
{
	union util_buf *buf;
	struct util_buf_footer *buf_ftr;

	buf = util_buf_alloc(pool);
	if (OFI_UNLIKELY(!buf))
		return NULL;

	buf_ftr = (struct util_buf_footer *) ((char *) buf + pool->attr.size);
	assert(context);
	*context = buf_ftr->region->context;
	return buf;
}

#if ENABLE_DEBUG
static inline int util_buf_use_ftr(struct util_buf_pool *pool)
{
	OFI_UNUSED(pool);
	return 1;
}
#else
static inline int util_buf_use_ftr(struct util_buf_pool *pool)
{
	return (pool->attr.alloc_hndlr ||
		pool->attr.free_hndlr ||
		pool->attr.use_ftr) ? 1 : 0;
}
#endif

static inline void *util_buf_get_ctx(struct util_buf_pool *pool, void *buf)
{
	struct util_buf_footer *buf_ftr;
	assert(util_buf_use_ftr(pool));
	buf_ftr = (struct util_buf_footer *) ((char *) buf + pool->attr.size);
	return buf_ftr->region->context;
}

void util_buf_pool_destroy(struct util_buf_pool *pool);


/*
 * Persistent memory support
 */
void ofi_pmem_init(void);

extern uint64_t OFI_RMA_PMEM;
extern void (*ofi_pmem_commit)(const void *addr, size_t len);


#endif /* _OFI_MEM_H_ */
