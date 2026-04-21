#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/crc32.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include "idkfs_kfs.h"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("idkfs team");
MODULE_DESCRIPTION("IDKFS: Zonal Tiered CoW FS");
static char *fast_exts = ".so,.bin,.exe,.o";
static char *slow_exts = ".iso,.tar,.zip,.gz,.bz2,.zst";
module_param(fast_exts, charp, 0644);
MODULE_PARM_DESC(fast_exts, "Comma-separated extension list for FAST tier");
module_param(slow_exts, charp, 0644);
MODULE_PARM_DESC(slow_exts, "Comma-separated extension list for SLOW tier");
static struct kmem_cache *idkfs_inode_cachep;
struct idkfs_sb_info {
	struct idkfs_super_block *isb;
	struct buffer_head *sbh;
	spinlock_t lock;
};
struct idkfs_inode_info {
	struct inode vfs_inode;
	struct idkfs_extent extents[IDKFS_MAX_EXTENTS];
	u8 tier;
};
static inline struct idkfs_inode_info *IDKFS_I(struct inode *inode) {
	return container_of(inode, struct idkfs_inode_info, vfs_inode);
}
static void idkfs_mark_sb_dirty(struct super_block *sb)
{
	struct idkfs_sb_info *sbi = sb->s_fs_info;
	if (sbi && sbi->sbh)
		mark_buffer_dirty(sbi->sbh);
}
static bool idkfs_ext_match(const char *name, const char *ext_csv) {
	char *list, *cur, *tok;
	bool matched = false;
	if (!name || !ext_csv)
		return false;
	list = kstrdup(ext_csv, GFP_KERNEL);
	if (!list)
		return false;
	cur = list;
	while ((tok = strsep(&cur, ",")) != NULL) {
		size_t nl, el;
		if (!*tok)
			continue;
		nl = strlen(name);
		el = strlen(tok);
		if (nl >= el && strncasecmp(name + nl - el, tok, el) == 0) {
			matched = true;
			break;
		}
	}
	kfree(list);
	return matched;
}
static u8 idkfs_get_tier(const char *name) {
	if (idkfs_ext_match(name, fast_exts))
		return TIER_FAST;
	if (idkfs_ext_match(name, slow_exts))
		return TIER_SLOW;
	return TIER_NORMAL;
}
static u64 idkfs_alloc_tiered(struct super_block *sb, u8 tier) {
	struct idkfs_sb_info *sbi = sb->s_fs_info;
	u64 phys;
	spin_lock(&sbi->lock);
	if (le64_to_cpu(sbi->isb->free_blocks) == 0) {
		spin_unlock(&sbi->lock);
		return 0;
	}
	if (tier == TIER_FAST) {
		phys = le64_to_cpu(sbi->isb->next_fast);
		sbi->isb->next_fast = cpu_to_le64(phys + 1);
	} else if (tier == TIER_SLOW) {
		phys = le64_to_cpu(sbi->isb->next_slow);
		sbi->isb->next_slow = cpu_to_le64(phys + 1);
	} else {
		phys = le64_to_cpu(sbi->isb->next_normal);
		sbi->isb->next_normal = cpu_to_le64(phys + 1);
	}
	sbi->isb->free_blocks = cpu_to_le64(le64_to_cpu(sbi->isb->free_blocks) - 1);
	mark_buffer_dirty(sbi->sbh);
	spin_unlock(&sbi->lock);
	return phys;
}
static int idkfs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create) {
	struct idkfs_inode_info *ii = IDKFS_I(inode);
	int i;
	for (i = 0; i < IDKFS_MAX_EXTENTS; i++) {
		u64 log = le64_to_cpu(ii->extents[i].logical);
		u32 len = le32_to_cpu(ii->extents[i].len);
		if (iblock >= log && iblock < log + len) {
			map_bh(bh_result, inode->i_sb, le64_to_cpu(ii->extents[i].physical) + (iblock - log));
			return 0;
		}
	}
	if (!create) return 0;
	u64 phys = idkfs_alloc_tiered(inode->i_sb, ii->tier);
	if (phys == 0)
		return -ENOSPC;
	for (i = 0; i < IDKFS_MAX_EXTENTS; i++) {
		if (ii->extents[i].len == 0) {
			ii->extents[i].logical = cpu_to_le64(iblock);
			ii->extents[i].physical = cpu_to_le64(phys);
			ii->extents[i].len = cpu_to_le32(1);
			break;
		}
	}
	map_bh(bh_result, inode->i_sb, phys);
	set_buffer_new(bh_result);
	return 0;
}
static int idkfs_write_begin(struct file *file, struct address_space *mapping, loff_t pos, unsigned len, unsigned flags, struct page **pagep, void **fsdata) {
	return block_write_begin(mapping, pos, len, flags, pagep, idkfs_get_block);
}
static int idkfs_write_inode(struct inode *inode, struct writeback_control *wbc) {
	struct idkfs_sb_info *sbi = inode->i_sb->s_fs_info;
	if (!sbi || !sbi->sbh)
		return -EIO;
	mark_buffer_dirty(sbi->sbh);
	if (wbc->sync_mode == WB_SYNC_ALL)
		sync_dirty_buffer(sbi->sbh);
	return 0;
}
static const struct address_space_operations idkfs_aops = {
	.readpage = mpage_readpage,
	.writepage = block_write_full_page,
	.write_begin = idkfs_write_begin,
	.write_end = generic_write_end,
};
static const struct file_operations idkfs_file_ops = {
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
	.mmap = generic_file_mmap,
	.fsync = generic_file_fsync,
	.splice_read = filemap_splice_read,
};
static const struct inode_operations idkfs_file_inode_ops = {
	.getattr = simple_getattr,
	.setattr = simple_setattr,
};
static const struct inode_operations idkfs_dir_inode_ops;
static int idkfs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
	struct inode *inode = new_inode(dir->i_sb);
	if (!inode) return -ENOMEM;
	IDKFS_I(inode)->tier = idkfs_get_tier(dentry->d_name.name);
	inode_init_owner(inode, dir, mode);
	inode->i_ino = get_next_ino();
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_blocks = 0;
	inode->i_mapping->a_ops = &idkfs_aops;
	inode->i_op = &idkfs_file_inode_ops;
	inode->i_fop = &idkfs_file_ops;
	mark_inode_dirty(inode);
	d_instantiate(dentry, inode);
	dir->i_mtime = dir->i_ctime = current_time(dir);
	mark_inode_dirty(dir);
	idkfs_mark_sb_dirty(dir->i_sb);
	return 0;
}
static int idkfs_unlink(struct inode *dir, struct dentry *dentry) {
	struct inode *inode = d_inode(dentry);
	inode->i_ctime = dir->i_ctime = dir->i_mtime = current_time(inode);
	drop_nlink(inode);
	mark_inode_dirty(inode);
	mark_inode_dirty(dir);
	idkfs_mark_sb_dirty(dir->i_sb);
	return 0;
}
static int idkfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode) {
	struct inode *inode = new_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;
	inode_init_owner(inode, dir, S_IFDIR | mode);
	inode->i_ino = get_next_ino();
	inode->i_op = &idkfs_dir_inode_ops;
	inode->i_fop = &simple_dir_operations;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	set_nlink(inode, 2);
	d_instantiate(dentry, inode);
	inc_nlink(dir);
	dir->i_mtime = dir->i_ctime = current_time(dir);
	mark_inode_dirty(dir);
	idkfs_mark_sb_dirty(dir->i_sb);
	return 0;
}
static int idkfs_rmdir(struct inode *dir, struct dentry *dentry) {
	if (!simple_empty(dentry))
		return -ENOTEMPTY;
	drop_nlink(d_inode(dentry));
	drop_nlink(dir);
	dir->i_ctime = dir->i_mtime = current_time(dir);
	mark_inode_dirty(dir);
	idkfs_mark_sb_dirty(dir->i_sb);
	return 0;
}
static int idkfs_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags) {
	int ret = simple_rename(old_dir, old_dentry, new_dir, new_dentry, flags);
	if (!ret) {
		idkfs_mark_sb_dirty(old_dir->i_sb);
		if (new_dir->i_sb != old_dir->i_sb)
			idkfs_mark_sb_dirty(new_dir->i_sb);
	}
	return ret;
}
static const struct inode_operations idkfs_dir_inode_ops = {
	.lookup = simple_lookup,
	.create = idkfs_create,
	.unlink = idkfs_unlink,
	.mkdir = idkfs_mkdir,
	.rmdir = idkfs_rmdir,
	.link = simple_link,
	.rename = idkfs_rename,
};
static struct inode *idkfs_alloc_inode(struct super_block *sb) {
	struct idkfs_inode_info *ii = kmem_cache_alloc(idkfs_inode_cachep, GFP_KERNEL);
	if (!ii) return NULL;
	memset(ii->extents, 0, sizeof(ii->extents));
	return &ii->vfs_inode;
}
static void idkfs_free_inode(struct inode *inode) {
	kmem_cache_free(idkfs_inode_cachep, IDKFS_I(inode));
}
static void idkfs_put_super(struct super_block *sb) {
	struct idkfs_sb_info *sbi = sb->s_fs_info;
	if (!sbi)
		return;
	if (sbi->sbh) {
		mark_buffer_dirty(sbi->sbh);
		sync_dirty_buffer(sbi->sbh);
		brelse(sbi->sbh);
	}
	kfree(sbi);
	sb->s_fs_info = NULL;
}
static const struct super_operations idkfs_super_ops = {
	.alloc_inode = idkfs_alloc_inode,
	.free_inode = idkfs_free_inode,
	.write_inode = idkfs_write_inode,
	.put_super = idkfs_put_super,
	.drop_inode = generic_drop_inode,
	.statfs = simple_statfs,
};
static int idkfs_fill_super(struct super_block *sb, void *data, int silent) {
	struct buffer_head *bh;
	struct idkfs_sb_info *sbi;
	struct inode *root;
	sb_set_blocksize(sb, IDKFS_BLOCK_SIZE);
	bh = sb_bread(sb, 0);
	if (!bh || le32_to_cpu(((struct idkfs_super_block *)bh->b_data)->magic) != IDKFS_MAGIC) {
		if (bh) brelse(bh);
		return -EINVAL;
	}
	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	sbi->isb = (struct idkfs_super_block *)bh->b_data;
	sbi->sbh = bh;
	spin_lock_init(&sbi->lock);
	sb->s_fs_info = sbi;
	sb->s_op = &idkfs_super_ops;
	root = new_inode(sb);
	root->i_ino = 1;
	root->i_mode = S_IFDIR | 0755;
	root->i_atime = root->i_mtime = root->i_ctime = current_time(root);
	set_nlink(root, 2);
	root->i_op = &idkfs_dir_inode_ops;
	root->i_fop = &simple_dir_operations;
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		idkfs_put_super(sb);
		return -ENOMEM;
	}
	return 0;
}
static struct dentry *idkfs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
	return mount_bdev(fs_type, flags, dev_name, data, idkfs_fill_super);
}
static struct file_system_type idkfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "idkfs",
	.mount = idkfs_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};
static int __init idkfs_init(void) {
	idkfs_inode_cachep = kmem_cache_create("idkfs_inode", sizeof(struct idkfs_inode_info), 0, SLAB_RECLAIM_ACCOUNT, NULL);
	return register_filesystem(&idkfs_fs_type);
}
static void __exit idkfs_exit(void) {
	unregister_filesystem(&idkfs_fs_type);
	kmem_cache_destroy(idkfs_inode_cachep);
}
module_init(idkfs_init);
module_exit(idkfs_exit);
#else
/*
 * This file is a Linux kernel module translation unit.
 * Keep a host-side stub so clangd on non-Linux/non-kernel environments
 * does not try to parse kernel-only headers and symbols.
 */
int idkfs_vfs_host_stub;
#endif
