#ifndef _IDKFS_KFS_H
#define _IDKFS_KFS_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/magic.h>
#else
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#endif

#define IDKFS_MAGIC 0x49444B46
#define IDKFS_BLOCK_SIZE 4096
#define IDKFS_MAX_EXTENTS 15
#define IDKFS_VERSION 2
#define IDKFS_TX_MAGIC 0x54584B46
#define IDKFS_SNAP_MAGIC 0x534E4B46
#define IDKFS_MAX_SNAPSHOTS 64
enum idkfs_tier { TIER_FAST = 0, TIER_NORMAL = 1, TIER_SLOW = 2 };
struct idkfs_extent {
	__le64 logical;
	__le64 physical;
	__le32 len;
	__le32 checksum;
} __attribute__((packed));
struct idkfs_super_block {
	__le32 magic;
	__le32 version;
	__le64 generation;
	__le64 root_node;
	__le64 next_fast;
	__le64 next_normal;
	__le64 next_slow;
	__le64 total_blocks;
	__le64 free_blocks;
	__le64 tx_region_block;
	__le64 snap_region_block;
	__u8 uuid[16];
	__le32 checksum;
	__u8 _pad[3988];
} __attribute__((packed));
struct idkfs_tx_record {
	__le32 magic;
	__le32 state;
	__le64 pending_generation;
	__le64 committed_generation;
	__le32 checksum;
	__u8 _pad[4068];
} __attribute__((packed));
struct idkfs_snapshot_entry {
	__le32 id;
	__le32 flags;
	__le64 generation;
	__le64 root_node;
	char name[64];
} __attribute__((packed));
struct idkfs_snapshot_table {
	__le32 magic;
	__le32 count;
	__le32 next_id;
	__le32 checksum;
	struct idkfs_snapshot_entry entries[IDKFS_MAX_SNAPSHOTS];
} __attribute__((packed));
struct idkfs_inode {
	__le32 ino;
	__le16 mode;
	__le16 nlink;
	__u8 tier;
	__u8 _pad;
	__le32 uid;
	__le32 gid;
	__le64 size;
	__le64 blocks;
	__le64 atime;
	__le64 mtime;
	__le64 ctime;
	__le64 generation;
	struct idkfs_extent extents[IDKFS_MAX_EXTENTS];
	__le32 checksum;
} __attribute__((packed));
#endif
