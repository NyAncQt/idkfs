#ifdef __KERNEL__
#include <linux/bio.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include "idkfs_kfs.h"

MODULE_AUTHOR("idkfs team");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Block driver that maps an idkfs image with journaling stubs");


#define IDKFS_BLOCK_SIZE 4096
#define IDKFS_DIRECT_BLOCKS 12
#define IDKFS_MAX_INDIRECT_LEVELS 6
#define IDKFS_PTRS_PER_BLOCK (IDKFS_BLOCK_SIZE / sizeof(uint64_t))
#define IDKFS_NULL_BLOCK (~0ULL)
#define IDKFS_JOURNAL_BLOCKS 64

struct idkfs_inode_disk {
	uint64_t direct[IDKFS_DIRECT_BLOCKS];
	uint64_t indirect[IDKFS_MAX_INDIRECT_LEVELS];
};

static uint64_t idkfs_pow(uint64_t base, uint8_t exp)
{
	uint64_t v = 1;
	while (exp--)
		v *= base;
	return v;
}

static uint64_t idkfs_indirect_span(uint8_t level)
{
	return idkfs_pow(IDKFS_PTRS_PER_BLOCK, level);
}

static bool idkfs_fill_chain(uint64_t logical, uint8_t *level_out,
			     uint64_t chain[IDKFS_MAX_INDIRECT_LEVELS])
{
	if (logical < IDKFS_DIRECT_BLOCKS) {
		*level_out = 0;
		chain[0] = logical;
		return true;
	}

	uint64_t rem = logical - IDKFS_DIRECT_BLOCKS;
	for (uint8_t lvl = 1; lvl <= IDKFS_MAX_INDIRECT_LEVELS; ++lvl) {
		uint64_t span = idkfs_indirect_span(lvl);
		if (rem < span) {
			*level_out = lvl;
			for (uint8_t i = 0; i < lvl; ++i) {
				uint64_t stride = idkfs_indirect_span(lvl - i - 1);
				chain[i] = rem / stride;
				rem %= stride;
			}
			return true;
		}
		rem -= span;
	}
	return false;
}

static uint64_t idkfs_safe_inode_read(uint64_t inode_block, size_t offset)
{
	resource_size_t byte = inode_block * IDKFS_BLOCK_SIZE;
	if (byte + sizeof(struct idkfs_inode_disk) > backing_size)
		return IDKFS_NULL_BLOCK;

	struct idkfs_inode_disk *inode;
	inode = (struct idkfs_inode_disk *)(backing_map + byte);
	if (!inode)
		return IDKFS_NULL_BLOCK;

	uint64_t *base = &inode->direct[0];
	return base[offset];
}

static uint64_t idkfs_safe_pointer_read(uint64_t block, uint64_t index)
{
	resource_size_t byte = block * IDKFS_BLOCK_SIZE;
	if (byte + IDKFS_BLOCK_SIZE > backing_size)
		return IDKFS_NULL_BLOCK;

	uint64_t *ptrs = (uint64_t *)(backing_map + byte);
	if (index >= IDKFS_PTRS_PER_BLOCK)
		return IDKFS_NULL_BLOCK;
	return ptrs[index];
}

static uint64_t idkfs_resolve_logical_block(uint64_t logical, uint64_t inode_block)
{
	uint64_t chain[IDKFS_MAX_INDIRECT_LEVELS] = {0};
	uint8_t level = 0;

	if (!idkfs_fill_chain(logical, &level, chain))
		return IDKFS_NULL_BLOCK;

	if (level == 0)
		return idkfs_safe_inode_read(inode_block, chain[0]);

	uint64_t current = idkfs_safe_inode_read(inode_block,
						 IDKFS_DIRECT_BLOCKS + level - 1);
	if (current == IDKFS_NULL_BLOCK)
		return IDKFS_NULL_BLOCK;

	for (uint8_t depth = 0; depth < level; ++depth) {
		current = idkfs_safe_pointer_read(current, chain[depth]);
		if (current == IDKFS_NULL_BLOCK)
			return IDKFS_NULL_BLOCK;
	}

	return current;
}


static char *backing_path = "myfs.img";
module_param(backing_path, charp, 0444);
MODULE_PARM_DESC(backing_path, "Path to the idkfs image (defaults to myfs.img)");

struct idkfs_blk_device {
	struct blk_mq_tag_set tag_set;
	struct request_queue *queue;
	struct gendisk *gd;
	spinlock_t queue_lock;

	struct delayed_work journal_work;
	bool journal_dirty;
};

static struct file *backing_file;
static void *backing_map;
static resource_size_t backing_size;
static int idkfs_major = 0;
static struct idkfs_blk_device *idkfs_dev;
static struct idkfs_super_block *idkfs_sb(void)
{
	return (struct idkfs_super_block *)backing_map;
}
static struct idkfs_tx_record *idkfs_tx(void)
{
	struct idkfs_super_block *sb;
	u64 tx_block;
	if (!backing_map)
		return NULL;
	sb = idkfs_sb();
	tx_block = le64_to_cpu(sb->tx_region_block);
	return (struct idkfs_tx_record *)(backing_map + (tx_block * IDKFS_BLOCK_SIZE));
}
static void idkfs_replay_if_needed(void)
{
	struct idkfs_tx_record *tx = idkfs_tx();
	struct idkfs_super_block *sb = idkfs_sb();
	u32 state;
	if (!tx || !sb)
		return;
	if (le32_to_cpu(tx->magic) != IDKFS_TX_MAGIC)
		return;
	state = le32_to_cpu(tx->state);
	if (state == 1) {
		sb->generation = tx->committed_generation;
		tx->state = cpu_to_le32(0);
		pr_warn("idkfs: recovered incomplete tx, rolled back to generation %llu\n",
			(unsigned long long)le64_to_cpu(sb->generation));
	}
}



static void idkfs_log_entry(sector_t sector, unsigned int len, bool is_write)
{
	pr_debug("%s: journal log entry sector=%llu len=%u write=%d\n", __func__,
		 (unsigned long long)sector, len, is_write);
}

static void idkfs_commit(struct work_struct *work)
{
	loff_t pos = 0;
	ssize_t written;

	if (!idkfs_dev)
		return;

	if (idkfs_dev->journal_dirty) {
		struct idkfs_tx_record *tx = idkfs_tx();
		struct idkfs_super_block *sb = idkfs_sb();
		u64 next_gen = 0;
		if (tx && sb && le32_to_cpu(tx->magic) == IDKFS_TX_MAGIC) {
			next_gen = le64_to_cpu(sb->generation) + 1;
			tx->pending_generation = cpu_to_le64(next_gen);
			tx->state = cpu_to_le32(1);
		}
		written = kernel_write(backing_file, backing_map, backing_size, &pos);
		if (written != (ssize_t)backing_size) {
			pr_err("idkfs: journal flush failed (%zd/%llu)\n", written,
			       (unsigned long long)backing_size);
		} else {
			if (tx && sb && le32_to_cpu(tx->magic) == IDKFS_TX_MAGIC) {
				if (next_gen == 0)
					next_gen = le64_to_cpu(sb->generation) + 1;
				sb->generation = cpu_to_le64(next_gen);
				tx->committed_generation = cpu_to_le64(next_gen);
				tx->state = cpu_to_le32(0);
			}
			vfs_fsync(backing_file, 0);
			idkfs_dev->journal_dirty = false;
		}
	}

	schedule_delayed_work(&idkfs_dev->journal_work, msecs_to_jiffies(250));
}

static void idkfs_rollback(void)
{
	pr_info("idkfs: rolling back journal (placeholder)\n");
	idkfs_dev->journal_dirty = false;
}


static int idkfs_map_backing(void)
{
	struct kstat st;
	loff_t pos = 0;
	ssize_t got;

	backing_file = filp_open(backing_path, O_RDWR | O_LARGEFILE, 0);
	if (IS_ERR(backing_file))
		return PTR_ERR(backing_file);

	if (vfs_getattr(&backing_file->f_path, &st, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT)) {
		pr_err("idkfs: failed to stat '%s'\n", backing_path);
		filp_close(backing_file, NULL);
		return -EIO;
	}

	backing_size = st.size;
	if (backing_size == 0) {
		pr_err("idkfs: backing image has zero size\n");
		filp_close(backing_file, NULL);
		return -EINVAL;
	}

	backing_map = vmalloc(backing_size);
	if (!backing_map) {
		pr_err("idkfs: vmalloc failed for backing map\n");
		filp_close(backing_file, NULL);
		return -ENOMEM;
	}

	got = kernel_read(backing_file, backing_map, backing_size, &pos);
	if (got != (ssize_t)backing_size) {
		pr_err("idkfs: kernel_read failed (%zd/%llu)\n", got,
		       (unsigned long long)backing_size);
		vfree(backing_map);
		backing_map = NULL;
		filp_close(backing_file, NULL);
		return got < 0 ? (int)got : -EIO;
	}
	idkfs_replay_if_needed();

	pr_info("idkfs: mapped '%s' (%llu bytes) at %p\n", backing_path,
		(unsigned long long)backing_size, backing_map);
	return 0;
}

static void idkfs_unmap_backing(void)
{
	if (backing_map)
		vfree(backing_map);
	if (backing_file)
		filp_close(backing_file, NULL);
	backing_map = NULL;
	backing_file = NULL;
	backing_size = 0;
}


static blk_status_t idkfs_blk_dispatch(struct blk_mq_hw_ctx *hctx,
				       const struct blk_mq_queue_data *bd)
{
	struct request *req = bd->rq;
	blk_status_t status = BLK_STS_OK;
	sector_t sector = blk_rq_pos(req);
	uint64_t offset = sector << 9;

	blk_mq_start_request(req);

	if (!backing_map || !backing_size) {
		status = BLK_STS_IOERR;
		goto done;
	}

	if (offset >= backing_size) {
		status = BLK_STS_IOERR;
		goto done;
	}

	if (req_op(req) != REQ_OP_READ && req_op(req) != REQ_OP_WRITE) {
		status = BLK_STS_IOERR;
		goto done;
	}

	struct req_iterator iter;
	struct bio_vec bv;
	rq_for_each_segment(bv, req, iter) {
		void *buffer = page_address(bv.bv_page) + bv.bv_offset;
		unsigned int len = bv.bv_len;

		if (offset + len > backing_size) {
			status = BLK_STS_IOERR;
			goto done;
		}

		void *target = backing_map + offset;
		if (req_op(req) == REQ_OP_READ)
			memcpy(buffer, target, len);
		else {
			memcpy(target, buffer, len);
			idkfs_log_entry(sector, len, true);
			idkfs_dev->journal_dirty = true;
		}

		sector += len >> 9;
		offset = sector << 9;
	}

done:
	blk_mq_end_request(req, status);
	return status;
}

static const struct blk_mq_ops idkfs_mq_ops = {
	.queue_rq = idkfs_blk_dispatch,
};


static int idkfs_blk_init_disk(void)
{
	int ret;

	idkfs_dev->queue = blk_mq_init_queue(&idkfs_dev->tag_set);
	if (IS_ERR(idkfs_dev->queue)) {
		ret = PTR_ERR(idkfs_dev->queue);
		idkfs_dev->queue = NULL;
		pr_err("idkfs: blk_mq_init_queue failed (%d)\n", ret);
		return ret;
	}

	spin_lock_init(&idkfs_dev->queue_lock);
	idkfs_dev->queue->queuedata = idkfs_dev;
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, idkfs_dev->queue);

	idkfs_dev->gd = alloc_disk(1);
	if (!idkfs_dev->gd) {
		pr_err("idkfs: alloc_disk failed\n");
		ret = -ENOMEM;
		goto cleanup_queue;
	}

	idkfs_dev->gd->major = idkfs_major;
	idkfs_dev->gd->first_minor = 0;
	idkfs_dev->gd->fops = NULL;
	idkfs_dev->gd->queue = idkfs_dev->queue;
	idkfs_dev->gd->private_data = idkfs_dev;
	strlcpy(idkfs_dev->gd->disk_name, "idkfsblk", DISK_NAME_LEN);
	set_capacity(idkfs_dev->gd, backing_size >> 9);
	add_disk(idkfs_dev->gd);
	return 0;

cleanup_queue:
	blk_cleanup_queue(idkfs_dev->queue);
	idkfs_dev->queue = NULL;
	return ret;
}


static int __init idkfs_blk_init(void)
{
	int ret;

	idkfs_dev = kzalloc(sizeof(*idkfs_dev), GFP_KERNEL);
	if (!idkfs_dev)
		return -ENOMEM;

	idkfs_dev->tag_set.ops = &idkfs_mq_ops;
	idkfs_dev->tag_set.queue_depth = 128;
	idkfs_dev->tag_set.numa_node = NUMA_NO_NODE;
	idkfs_dev->tag_set.cmd_size = 0;
	idkfs_dev->tag_set.driver_data = idkfs_dev;
	idkfs_dev->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;

	ret = idkfs_map_backing();
	if (ret)
		goto err_map;

	idkfs_major = register_blkdev(0, "idkfsblk");
	if (idkfs_major < 0) {
		pr_err("idkfs: register_blkdev failed (%d)\n", idkfs_major);
		ret = idkfs_major;
		goto err_major;
	}

	ret = blk_mq_alloc_tag_set(&idkfs_dev->tag_set);
	if (ret)
		goto err_tagset;

	ret = idkfs_blk_init_disk();
	if (ret)
		goto err_disk;

	INIT_DELAYED_WORK(&idkfs_dev->journal_work, idkfs_commit);
	schedule_delayed_work(&idkfs_dev->journal_work, msecs_to_jiffies(250));
	pr_info("idkfs: block device ready (major=%d)\n", idkfs_major);
	return 0;

err_disk:
	blk_mq_free_tag_set(&idkfs_dev->tag_set);
err_tagset:
	unregister_blkdev(idkfs_major, "idkfsblk");
err_major:
	idkfs_unmap_backing();
err_map:
	kfree(idkfs_dev);
	idkfs_dev = NULL;
	return ret;
}

static void __exit idkfs_blk_exit(void)
{
	if (!idkfs_dev)
		return;

	cancel_delayed_work_sync(&idkfs_dev->journal_work);

	if (idkfs_dev->gd) {
		del_gendisk(idkfs_dev->gd);
		put_disk(idkfs_dev->gd);
	}

	if (idkfs_dev->queue)
		blk_cleanup_queue(idkfs_dev->queue);

	blk_mq_free_tag_set(&idkfs_dev->tag_set);

	if (idkfs_major > 0)
		unregister_blkdev(idkfs_major, "idkfsblk");

	idkfs_unmap_backing();
	kfree(idkfs_dev);
	idkfs_dev = NULL;
	pr_info("idkfs: block device unloaded\n");
}

module_init(idkfs_blk_init);
module_exit(idkfs_blk_exit);
#else
/*
 * Linux kernel module source: host-side stub for non-kernel tooling.
 */
int idkfs_blk_host_stub;
#endif
