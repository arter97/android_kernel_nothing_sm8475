// SPDX-License-Identifier: GPL-2.0
/*
 * Data verification functions, i.e. hooks for ->readpages()
 *
 * Copyright 2019 Google LLC
 */

#include "fsverity_private.h"

#include <crypto/hash.h>
#include <linux/bio.h>

static struct workqueue_struct *fsverity_read_workqueue;

/*
 * Returns true if the hash block with index @hblock_idx in the tree, located in
 * @hpage, has already been verified.
 */
static bool is_hash_block_verified(struct fsverity_info *vi, struct page *hpage,
				   unsigned long hblock_idx)
{
	unsigned int blocks_per_page;
	unsigned int i;

	/*
	 * When the Merkle tree block size and page size are the same, then the
	 * ->hash_block_verified bitmap isn't allocated, and we use PG_checked
	 * to directly indicate whether the page's block has been verified.
	 *
	 * Using PG_checked also guarantees that we re-verify hash pages that
	 * get evicted and re-instantiated from the backing storage, as new
	 * pages always start out with PG_checked cleared.
	 */
	if (!vi->hash_block_verified)
		return PageChecked(hpage);

	/*
	 * When the Merkle tree block size and page size differ, we use a bitmap
	 * to indicate whether each hash block has been verified.
	 *
	 * However, we still need to ensure that hash pages that get evicted and
	 * re-instantiated from the backing storage are re-verified.  To do
	 * this, we use PG_checked again, but now it doesn't really mean
	 * "checked".  Instead, now it just serves as an indicator for whether
	 * the hash page is newly instantiated or not.  If the page is new, as
	 * indicated by PG_checked=0, we clear the bitmap bits for the page's
	 * blocks since they are untrustworthy, then set PG_checked=1.
	 * Otherwise we return the bitmap bit for the requested block.
	 *
	 * Multiple threads may execute this code concurrently on the same page.
	 * This is safe because we use memory barriers to ensure that if a
	 * thread sees PG_checked=1, then it also sees the associated bitmap
	 * clearing to have occurred.  Also, all writes and their corresponding
	 * reads are atomic, and all writes are safe to repeat in the event that
	 * multiple threads get into the PG_checked=0 section.  (Clearing a
	 * bitmap bit again at worst causes a hash block to be verified
	 * redundantly.  That event should be very rare, so it's not worth using
	 * a lock to avoid.  Setting PG_checked again has no effect.)
	 */
	if (PageChecked(hpage)) {
		/*
		 * A read memory barrier is needed here to give ACQUIRE
		 * semantics to the above PageChecked() test.
		 */
		smp_rmb();
		return test_bit(hblock_idx, vi->hash_block_verified);
	}
	blocks_per_page = vi->tree_params.blocks_per_page;
	hblock_idx = round_down(hblock_idx, blocks_per_page);
	for (i = 0; i < blocks_per_page; i++)
		clear_bit(hblock_idx + i, vi->hash_block_verified);
	/*
	 * A write memory barrier is needed here to give RELEASE semantics to
	 * the below SetPageChecked() operation.
	 */
	smp_wmb();
	SetPageChecked(hpage);
	return false;
}

/*
 * Verify the hash of a single data block against the file's Merkle tree.
 *
 * @real_dblock_hash specifies the hash of the data block, and @data_pos
 * specifies the byte position of the data block within the file.
 *
 * In principle, we need to verify the entire path to the root node.  However,
 * for efficiency the filesystem may cache the hash blocks.  Therefore we need
 * only ascend the tree until an already-verified hash block is seen, and then
 * verify the path to that block.
 *
 * Return: %true if the data block is valid, else %false.
 */
static bool
verify_data_block(struct inode *inode, struct fsverity_info *vi,
		  const u8 *real_dblock_hash, u64 data_pos,
		  unsigned long max_ra_pages)
{
	const struct merkle_tree_params *params = &vi->tree_params;
	const unsigned int hsize = params->digest_size;
	int level;
	u8 _want_hash[FS_VERITY_MAX_DIGEST_SIZE];
	const u8 *want_hash;
	u8 real_hblock_hash[FS_VERITY_MAX_DIGEST_SIZE];
	/* The hash blocks that are traversed, indexed by level */
	struct {
		/* Page containing the hash block */
		struct page *page;
		/* Mapped address of the hash block (will be within @page) */
		void *addr;
		/* Index of the hash block in the tree overall */
		unsigned long index;
		/* Byte offset of the wanted hash relative to @addr */
		unsigned int hoffset;
	} hblocks[FS_VERITY_MAX_LEVELS];
	/*
	 * The index of the previous level's block within that level; also the
	 * index of that block's hash within the current level.
	 */
	u64 hidx = data_pos >> params->log_blocksize;

	if (unlikely(data_pos >= inode->i_size)) {
		/*
		 * This can happen in the data page spanning EOF when the Merkle
		 * tree block size is less than the page size.  The Merkle tree
		 * doesn't cover data blocks fully past EOF.  But the entire
		 * page spanning EOF can be visible to userspace via a mmap, and
		 * any part past EOF should be all zeroes.  Therefore, we need
		 * to verify that any data blocks fully past EOF are all zeroes.
		 */
		if (memcmp(vi->zero_block_hash, real_dblock_hash,
			   params->block_size) != 0) {
			fsverity_err(inode,
				     "FILE CORRUPTED!  Data past EOF is not zeroed");
			return false;
		}
		return true;
	}

	/*
	 * Starting at the leaf level, ascend the tree saving hash blocks along
	 * the way until we find a hash block that has already been verified, or
	 * until we reach the root.
	 */
	for (level = 0; level < params->num_levels; level++) {
		unsigned long next_hidx;
		unsigned long hblock_idx;
		pgoff_t hpage_idx;
		unsigned int hblock_offset_in_page;
		unsigned int hoffset;
		struct page *hpage;
		void *haddr;

		/*
		 * The index of the block in the current level; also the index
		 * of that block's hash within the next level.
		 */
		next_hidx = hidx >> params->log_arity;

		/* Index of the hash block in the tree overall */
		hblock_idx = params->level_start[level] + next_hidx;

		/* Index of the hash page in the tree overall */
		hpage_idx = hblock_idx >> params->log_blocks_per_page;

		/* Byte offset of the hash block within the page */
		hblock_offset_in_page =
			(hblock_idx << params->log_blocksize) & ~PAGE_MASK;

		/* Byte offset of the hash within the block */
		hoffset = (hidx << params->log_digestsize) &
			  (params->block_size - 1);

		hpage = inode->i_sb->s_vop->read_merkle_tree_page(inode,
				hpage_idx, level == 0 ? min(max_ra_pages,
					params->tree_pages - hpage_idx) : 0);
		if (IS_ERR(hpage)) {
			fsverity_err(inode,
				     "Error %ld reading Merkle tree page %lu",
				     PTR_ERR(hpage), hpage_idx);
			goto error;
		}
		haddr = kmap_local_page(hpage) + hblock_offset_in_page;
		if (is_hash_block_verified(vi, hpage, hblock_idx)) {
			memcpy(_want_hash, haddr + hoffset, hsize);
			want_hash = _want_hash;
			kunmap_local(haddr);
			put_page(hpage);
			goto descend;
		}
		hblocks[level].page = hpage;
		hblocks[level].addr = haddr;
		hblocks[level].index = hblock_idx;
		hblocks[level].hoffset = hoffset;
		hidx = next_hidx;
	}

	want_hash = vi->root_hash;
descend:
	/* Descend the tree verifying hash blocks. */
	for (; level > 0; level--) {
		struct page *hpage = hblocks[level - 1].page;
		void *haddr = hblocks[level - 1].addr;
		unsigned long hblock_idx = hblocks[level - 1].index;
		unsigned int hoffset = hblocks[level - 1].hoffset;

		if (fsverity_hash_block(params, inode, haddr,
					real_hblock_hash) != 0)
			goto error;
		if (memcmp(want_hash, real_hblock_hash, hsize) != 0)
			goto corrupted;
		/*
		 * Mark the hash block as verified.  This must be atomic and
		 * idempotent, as the same hash block might be verified by
		 * multiple threads concurrently.
		 */
		if (vi->hash_block_verified)
			set_bit(hblock_idx, vi->hash_block_verified);
		else
			SetPageChecked(hpage);
		memcpy(_want_hash, haddr + hoffset, hsize);
		want_hash = _want_hash;
		kunmap_local(haddr);
		put_page(hpage);
	}

	/* Finally, verify the hash of the data block. */
	if (memcmp(want_hash, real_dblock_hash, hsize) != 0)
		goto corrupted;
	return true;

corrupted:
	fsverity_err(inode,
		     "FILE CORRUPTED! pos=%llu, level=%d, want_hash=%s:%*phN, real_hash=%s:%*phN",
		     data_pos, level - 1,
		     params->hash_alg->name, hsize, want_hash,
		     params->hash_alg->name, hsize,
		     level == 0 ? real_dblock_hash : real_hblock_hash);
error:
	for (; level > 0; level--) {
		kunmap_local(hblocks[level - 1].addr);
		put_page(hblocks[level - 1].page);
	}
	return false;
}

struct fsverity_verification_context {
	struct inode *inode;
	struct fsverity_info *vi;
	unsigned long max_ra_pages;

	/*
	 * pending_data and pending_pos are used when the selected hash
	 * algorithm supports multibuffer hashing.  They're used to temporarily
	 * store the virtual address and position of a mapped data block that
	 * needs to be verified.  If we then see another data block, we hash the
	 * two blocks simultaneously using the fast multibuffer hashing method.
	 */
	void *pending_data;
	u64 pending_pos;

	/* Buffers to temporarily store the calculated data block hashes */
	u8 hash1[FS_VERITY_MAX_DIGEST_SIZE];
	u8 hash2[FS_VERITY_MAX_DIGEST_SIZE];
};

static inline void
fsverity_init_verification_context(struct fsverity_verification_context *ctx,
				   struct inode *inode,
				   unsigned long max_ra_pages)
{
	ctx->inode = inode;
	ctx->vi = inode->i_verity_info;
	ctx->max_ra_pages = max_ra_pages;
	ctx->pending_data = NULL;
}

static bool
fsverity_finish_verification(struct fsverity_verification_context *ctx)
{
	int err;

	if (ctx->pending_data == NULL)
		return true;
	/*
	 * Multibuffer hashing is enabled but there was an odd number of data
	 * blocks.  Hash and verify the last block by itself.
	 */
	err = fsverity_hash_block(&ctx->vi->tree_params, ctx->inode,
				  ctx->pending_data, ctx->hash1);
	kunmap_local(ctx->pending_data);
	ctx->pending_data = NULL;
	return err == 0 &&
	       verify_data_block(ctx->inode, ctx->vi, ctx->hash1,
				 ctx->pending_pos, ctx->max_ra_pages);
}

static inline void
fsverity_abort_verification(struct fsverity_verification_context *ctx)
{
	if (ctx->pending_data) {
		kunmap_local(ctx->pending_data);
		ctx->pending_data = NULL;
	}
}

static bool
fsverity_add_data_blocks(struct fsverity_verification_context *ctx,
			 struct page *data_page, size_t len, size_t offset)
{
	struct inode *inode = ctx->inode;
	struct fsverity_info *vi = ctx->vi;
	const struct merkle_tree_params *params = &vi->tree_params;
	const unsigned int block_size = params->block_size;
	const bool multibuffer = params->hash_alg->supports_multibuffer;
	u64 pos = ((u64)data_page->index << PAGE_SHIFT) + offset;

	if (WARN_ON_ONCE(len <= 0 || !IS_ALIGNED(len | offset, block_size)))
		return false;
	if (WARN_ON_ONCE(!PageLocked(data_page) || PageUptodate(data_page)))
		return false;
	do {
		void *data = kmap_local_page(data_page);
		int err;

		if (multibuffer) {
			if (ctx->pending_data) {
				/* Hash and verify two data blocks. */
				err = fsverity_hash_2_blocks(params,
							     inode,
							     ctx->pending_data,
							     data,
							     ctx->hash1,
							     ctx->hash2);
				kunmap_local(data);
				kunmap_local(ctx->pending_data);
				ctx->pending_data = NULL;
				if (err != 0 ||
				    !verify_data_block(inode, vi, ctx->hash1,
						       ctx->pending_pos,
						       ctx->max_ra_pages) ||
				    !verify_data_block(inode, vi, ctx->hash2,
						       pos, ctx->max_ra_pages))
					return false;
			} else {
				/* Wait and see if there's another block. */
				ctx->pending_data = data;
				ctx->pending_pos = pos;
			}
		} else {
			/* Hash and verify one data block. */
			err = fsverity_hash_block(params, inode, data,
						  ctx->hash1);
			kunmap_local(data);
			if (err != 0 ||
			    !verify_data_block(inode, vi, ctx->hash1,
					       pos, ctx->max_ra_pages))
				return false;
		}
		pos += block_size;
		offset += block_size;
		len -= block_size;
	} while (len);
	return true;
}

/**
 * fsverity_verify_blocks() - verify data in a page
 * @page: the page containing the data to verify
 * @len: the length of the data to verify in the page
 * @offset: the offset of the data to verify in the page
 *
 * Verify data that has just been read from a verity file.  The data must be
 * located in a pagecache page that is still locked and not yet uptodate.  The
 * length and offset of the data must be Merkle tree block size aligned.
 *
 * Return: %true if the data is valid, else %false.
 */
bool fsverity_verify_blocks(struct page *page, unsigned int len,
			    unsigned int offset)
{
	struct fsverity_verification_context ctx;

	fsverity_init_verification_context(&ctx, page->mapping->host, 0);

	if (!fsverity_add_data_blocks(&ctx, page, len, offset)) {
		fsverity_abort_verification(&ctx);
		return false;
	}
	return fsverity_finish_verification(&ctx);
}
EXPORT_SYMBOL_GPL(fsverity_verify_blocks);

#ifdef CONFIG_BLOCK
/**
 * fsverity_verify_bio() - verify a 'read' bio that has just completed
 * @bio: the bio to verify
 *
 * Verify the bio's data against the file's Merkle tree.  All bio data segments
 * must be aligned to the file's Merkle tree block size.  If any data fails
 * verification, then bio->bi_status is set to an error status.
 *
 * This is a helper function for use by the ->readpages() method of filesystems
 * that issue bios to read data directly into the page cache.  Filesystems that
 * populate the page cache without issuing bios (e.g. non block-based
 * filesystems) must instead call fsverity_verify_page() directly on each page.
 * All filesystems must also call fsverity_verify_page() on holes.
 */
void fsverity_verify_bio(struct bio *bio)
{
	struct inode *inode = bio_first_page_all(bio)->mapping->host;
	struct fsverity_verification_context ctx;
	struct bio_vec *bv;
	struct bvec_iter_all iter_all;
	unsigned long max_ra_pages = 0;

	if (bio->bi_opf & REQ_RAHEAD) {
		/*
		 * If this bio is for data readahead, then we also do readahead
		 * of the first (largest) level of the Merkle tree.  Namely,
		 * when a Merkle tree page is read, we also try to piggy-back on
		 * some additional pages -- up to 1/4 the number of data pages.
		 *
		 * This improves sequential read performance, as it greatly
		 * reduces the number of I/O requests made to the Merkle tree.
		 */
		max_ra_pages = bio->bi_iter.bi_size >> (PAGE_SHIFT + 2);
	}

	fsverity_init_verification_context(&ctx, inode, max_ra_pages);

	bio_for_each_segment_all(bv, bio, iter_all) {
		if (!fsverity_add_data_blocks(&ctx, bv->bv_page, bv->bv_len,
					      bv->bv_offset)) {
			fsverity_abort_verification(&ctx);
			goto ioerr;
		}
	}

	if (!fsverity_finish_verification(&ctx))
		goto ioerr;
	return;

ioerr:
	bio->bi_status = BLK_STS_IOERR;
}
EXPORT_SYMBOL_GPL(fsverity_verify_bio);
#endif /* CONFIG_BLOCK */

/**
 * fsverity_enqueue_verify_work() - enqueue work on the fs-verity workqueue
 * @work: the work to enqueue
 *
 * Enqueue verification work for asynchronous processing.
 */
void fsverity_enqueue_verify_work(struct work_struct *work)
{
	queue_work(fsverity_read_workqueue, work);
}
EXPORT_SYMBOL_GPL(fsverity_enqueue_verify_work);

int __init fsverity_init_workqueue(void)
{
	/*
	 * Use a high-priority workqueue to prioritize verification work, which
	 * blocks reads from completing, over regular application tasks.
	 *
	 * For performance reasons, don't use an unbound workqueue.  Using an
	 * unbound workqueue for crypto operations causes excessive scheduler
	 * latency on ARM64.
	 */
	fsverity_read_workqueue = alloc_workqueue("fsverity_read_queue",
						  WQ_HIGHPRI,
						  num_online_cpus());
	if (!fsverity_read_workqueue)
		return -ENOMEM;
	return 0;
}

void __init fsverity_exit_workqueue(void)
{
	destroy_workqueue(fsverity_read_workqueue);
	fsverity_read_workqueue = NULL;
}
