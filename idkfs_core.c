/*
 * idkfs_core.c — I Don't Know Filesystem
 * NOTE: no main() — included by idkfs_fuse.c
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IDKFS_MAGIC 0x49444B46
#define IDKFS_VERSION 1
#define IDKFS_BLOCK_SIZE 4096
#define IDKFS_TOTAL_BLOCKS 65536
#define IDKFS_MAX_INODES 4096
#define IDKFS_MAX_FILENAME 255
#define IDKFS_DIRECT_BLOCKS 12
#define IDKFS_MAX_DIRENTS 128
#define IDKFS_ROOT_INODE 0
#define IDKFS_NULL_BLOCK UINT32_MAX
#define IDKFS_NULL_INODE UINT32_MAX

typedef enum { TIER_FAST = 0, TIER_NORMAL = 1, TIER_SLOW = 2 } SpeedTier;
typedef enum {
  IDKFS_FT_UNKNOWN = 0,
  IDKFS_FT_FILE = 1,
  IDKFS_FT_DIR = 2,
  IDKFS_FT_SYMLINK = 3,
  IDKFS_FT_DEVICE = 4
} FileType;

typedef struct {
  bool journaling, checksums, timestamps, compression;
  bool encryption, dedup, prefetch, lua_hooks, sorting;
} FeatureFlags;

static inline FeatureFlags features_default(void) {
  return (FeatureFlags){.journaling = true,
                        .checksums = true,
                        .timestamps = true,
                        .prefetch = true,
                        .lua_hooks = true,
                        .sorting = true};
}
static inline FeatureFlags features_ludicrous_speed(void) {
  return (FeatureFlags){0};
}

typedef struct __attribute__((packed)) {
  uint32_t magic, version, block_size, total_blocks, free_blocks;
  uint32_t total_inodes, free_inodes, inode_table_block, bitmap_block;
  uint32_t data_start_block, root_inode;
  uint64_t created_at, last_mounted, last_written;
  uint32_t mount_count, flags;
  uint8_t uuid[16];
  char label[64];
  uint32_t checksum;
  uint8_t _pad[3820];
} Superblock;

typedef struct __attribute__((packed)) {
  uint32_t ino;
  uint8_t type, tier;
  uint16_t mode;
  uint32_t uid, gid;
  uint64_t size, atime, mtime, ctime;
  uint32_t link_count, block_count;
  uint32_t direct[IDKFS_DIRECT_BLOCKS];
  uint32_t indirect1, indirect2, flags, checksum;
  uint8_t _pad[16];
} Inode;

typedef struct __attribute__((packed)) {
  uint32_t inode;
  uint8_t type, name_len;
  char name[IDKFS_MAX_FILENAME];
} DirEntry;

typedef struct {
  uint32_t count;
  DirEntry entries[IDKFS_MAX_DIRENTS];
} Directory;

#define BITMAP_SIZE (IDKFS_TOTAL_BLOCKS / 8)
typedef struct {
  uint8_t bits[BITMAP_SIZE];
} BlockBitmap;

typedef struct {
  Superblock sb;
  BlockBitmap bitmap;
  Inode inodes[IDKFS_MAX_INODES];
  uint8_t *disk;
  size_t disk_size;
  FeatureFlags features;
  bool dirty;
} IDKFS;

static uint32_t crc32_table[256];
static bool crc32_initialized = false;
static void crc32_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int j = 0; j < 8; j++)
      c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
    crc32_table[i] = c;
  }
  crc32_initialized = true;
}
static uint32_t crc32(const void *data, size_t len) {
  if (!crc32_initialized)
    crc32_init();
  const uint8_t *p = data;
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++)
    c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFF;
}

static void bitmap_set(BlockBitmap *bm, uint32_t b) {
  bm->bits[b / 8] |= (1 << (b % 8));
}
static void bitmap_clear(BlockBitmap *bm, uint32_t b) {
  bm->bits[b / 8] &= ~(1 << (b % 8));
}
static bool bitmap_test(const BlockBitmap *bm, uint32_t b) {
  return (bm->bits[b / 8] >> (b % 8)) & 1;
}
static int32_t bitmap_alloc(BlockBitmap *bm, uint32_t hint) {
  for (uint32_t i = hint; i < IDKFS_TOTAL_BLOCKS; i++)
    if (!bitmap_test(bm, i)) {
      bitmap_set(bm, i);
      return (int32_t)i;
    }
  for (uint32_t i = 0; i < hint; i++)
    if (!bitmap_test(bm, i)) {
      bitmap_set(bm, i);
      return (int32_t)i;
    }
  return -1;
}

static int32_t inode_alloc(IDKFS *fs) {
  for (uint32_t i = 1; i < IDKFS_MAX_INODES; i++) {
    if (fs->inodes[i].link_count == 0) {
      memset(&fs->inodes[i], 0, sizeof(Inode));
      fs->inodes[i].ino = i;
      for (int j = 0; j < IDKFS_DIRECT_BLOCKS; j++)
        fs->inodes[i].direct[j] = IDKFS_NULL_BLOCK;
      fs->inodes[i].indirect1 = fs->inodes[i].indirect2 = IDKFS_NULL_BLOCK;
      fs->sb.free_inodes--;
      return (int32_t)i;
    }
  }
  return -1;
}
static void inode_free(IDKFS *fs, uint32_t ino) {
  memset(&fs->inodes[ino], 0, sizeof(Inode));
  fs->sb.free_inodes++;
}

static uint8_t *block_ptr(IDKFS *fs, uint32_t b) {
  return fs->disk + ((size_t)b * IDKFS_BLOCK_SIZE);
}
static void block_read(IDKFS *fs, uint32_t b, void *buf) {
  memcpy(buf, block_ptr(fs, b), IDKFS_BLOCK_SIZE);
}
static void block_write(IDKFS *fs, uint32_t b, const void *buf) {
  memcpy(block_ptr(fs, b), buf, IDKFS_BLOCK_SIZE);
  fs->dirty = true;
}
static void block_zero(IDKFS *fs, uint32_t b) {
  memset(block_ptr(fs, b), 0, IDKFS_BLOCK_SIZE);
}

static uint32_t inode_get_block(IDKFS *fs, Inode *ino, uint32_t logical) {
  if (logical < IDKFS_DIRECT_BLOCKS)
    return ino->direct[logical];
  uint32_t ppb = IDKFS_BLOCK_SIZE / sizeof(uint32_t),
           idx = logical - IDKFS_DIRECT_BLOCKS;
  if (idx < ppb) {
    if (ino->indirect1 == IDKFS_NULL_BLOCK)
      return IDKFS_NULL_BLOCK;
    uint32_t ptrs[ppb];
    block_read(fs, ino->indirect1, ptrs);
    return ptrs[idx];
  }
  return IDKFS_NULL_BLOCK;
}

static int32_t inode_map_block(IDKFS *fs, Inode *ino, uint32_t logical) {
  int32_t nb = bitmap_alloc(&fs->bitmap, fs->sb.data_start_block);
  if (nb < 0)
    return -1;
  block_zero(fs, (uint32_t)nb);
  fs->sb.free_blocks--;
  if (logical < IDKFS_DIRECT_BLOCKS) {
    ino->direct[logical] = (uint32_t)nb;
    return nb;
  }
  uint32_t ppb = IDKFS_BLOCK_SIZE / sizeof(uint32_t),
           idx = logical - IDKFS_DIRECT_BLOCKS;
  if (idx < ppb) {
    if (ino->indirect1 == IDKFS_NULL_BLOCK) {
      int32_t ib = bitmap_alloc(&fs->bitmap, fs->sb.data_start_block);
      if (ib < 0)
        return -1;
      block_zero(fs, (uint32_t)ib);
      ino->indirect1 = (uint32_t)ib;
      fs->sb.free_blocks--;
    }
    uint32_t ptrs[ppb];
    block_read(fs, ino->indirect1, ptrs);
    ptrs[idx] = (uint32_t)nb;
    block_write(fs, ino->indirect1, ptrs);
    return nb;
  }
  return -1;
}

static ssize_t idkfs_read(IDKFS *fs, Inode *ino, void *buf, size_t count,
                          uint64_t offset) {
  if (offset >= ino->size)
    return 0;
  if (offset + count > ino->size)
    count = ino->size - offset;
  uint8_t *out = buf;
  size_t total = 0;
  while (total < count) {
    uint32_t logical = (uint32_t)((offset + total) / IDKFS_BLOCK_SIZE);
    uint32_t blk_off = (uint32_t)((offset + total) % IDKFS_BLOCK_SIZE);
    uint32_t can = (uint32_t)(IDKFS_BLOCK_SIZE - blk_off);
    if (can > count - total)
      can = (uint32_t)(count - total);
    uint32_t db = inode_get_block(fs, ino, logical);
    if (db == IDKFS_NULL_BLOCK)
      break;
    uint8_t tmp[IDKFS_BLOCK_SIZE];
    block_read(fs, db, tmp);
    memcpy(out + total, tmp + blk_off, can);
    total += can;
  }
  if (fs->features.timestamps)
    ino->atime = (uint64_t)time(NULL);
  return (ssize_t)total;
}

static ssize_t idkfs_write(IDKFS *fs, Inode *ino, const void *buf, size_t count,
                           uint64_t offset) {
  const uint8_t *in = buf;
  size_t total = 0;
  while (total < count) {
    uint32_t logical = (uint32_t)((offset + total) / IDKFS_BLOCK_SIZE);
    uint32_t blk_off = (uint32_t)((offset + total) % IDKFS_BLOCK_SIZE);
    uint32_t can = (uint32_t)(IDKFS_BLOCK_SIZE - blk_off);
    if (can > count - total)
      can = (uint32_t)(count - total);
    uint32_t db = inode_get_block(fs, ino, logical);
    if (db == IDKFS_NULL_BLOCK) {
      int32_t nb = inode_map_block(fs, ino, logical);
      if (nb < 0)
        return -1;
      db = (uint32_t)nb;
      ino->block_count++;
    }
    uint8_t tmp[IDKFS_BLOCK_SIZE];
    block_read(fs, db, tmp);
    memcpy(tmp + blk_off, in + total, can);
    block_write(fs, db, tmp);
    total += can;
  }
  if (offset + count > ino->size)
    ino->size = offset + count;
  if (fs->features.timestamps) {
    uint64_t n = (uint64_t)time(NULL);
    ino->mtime = ino->ctime = n;
  }
  if (fs->features.checksums)
    ino->checksum = crc32(ino, sizeof(Inode) - sizeof(uint32_t));
  fs->dirty = true;
  return (ssize_t)total;
}

static int idkfs_dir_add(IDKFS *fs, Inode *dir_ino, const char *name,
                         uint32_t ino, FileType type) {
  Directory dir;
  if (dir_ino->size == 0)
    memset(&dir, 0, sizeof(dir));
  else
    idkfs_read(fs, dir_ino, &dir, sizeof(dir), 0);
  if (dir.count >= IDKFS_MAX_DIRENTS)
    return -1;
  DirEntry *e = &dir.entries[dir.count++];
  e->inode = ino;
  e->type = (uint8_t)type;
  e->name_len = (uint8_t)strlen(name);
  strncpy(e->name, name, IDKFS_MAX_FILENAME);
  idkfs_write(fs, dir_ino, &dir, sizeof(dir), 0);
  return 0;
}

static int32_t idkfs_dir_lookup(IDKFS *fs, Inode *dir_ino, const char *name) {
  if (dir_ino->size == 0)
    return -1;
  Directory dir;
  idkfs_read(fs, dir_ino, &dir, sizeof(dir), 0);
  for (uint32_t i = 0; i < dir.count; i++)
    if (strncmp(dir.entries[i].name, name, IDKFS_MAX_FILENAME) == 0)
      return (int32_t)dir.entries[i].inode;
  return -1;
}

static int32_t idkfs_create(IDKFS *fs, uint32_t parent, const char *name,
                            SpeedTier tier, uint16_t mode) {
  int32_t ino = inode_alloc(fs);
  if (ino < 0)
    return -1;
  Inode *i = &fs->inodes[ino];
  i->type = IDKFS_FT_FILE;
  i->tier = (uint8_t)tier;
  i->mode = mode;
  i->link_count = 1;
  i->atime = i->mtime = i->ctime = (uint64_t)time(NULL);
  if (idkfs_dir_add(fs, &fs->inodes[parent], name, (uint32_t)ino,
                    IDKFS_FT_FILE) < 0) {
    inode_free(fs, (uint32_t)ino);
    return -1;
  }
  return ino;
}

static int32_t idkfs_mkdir(IDKFS *fs, uint32_t parent, const char *name,
                           uint16_t mode) {
  int32_t ino = inode_alloc(fs);
  if (ino < 0)
    return -1;
  Inode *i = &fs->inodes[ino];
  i->type = IDKFS_FT_DIR;
  i->tier = TIER_NORMAL;
  i->mode = mode;
  i->link_count = 2;
  i->atime = i->mtime = i->ctime = (uint64_t)time(NULL);
  idkfs_dir_add(fs, i, ".", (uint32_t)ino, IDKFS_FT_DIR);
  idkfs_dir_add(fs, i, "..", parent, IDKFS_FT_DIR);
  idkfs_dir_add(fs, &fs->inodes[parent], name, (uint32_t)ino, IDKFS_FT_DIR);
  fs->inodes[parent].link_count++;
  return ino;
}

static void idkfs_flush(IDKFS *fs) {
  if (!fs->dirty)
    return;
  if (fs->features.checksums)
    fs->sb.checksum = crc32(&fs->sb, sizeof(Superblock) - sizeof(uint32_t));
  memcpy(fs->disk, &fs->sb, sizeof(Superblock));
  memcpy(fs->disk + IDKFS_BLOCK_SIZE, &fs->bitmap, sizeof(BlockBitmap));
  memcpy(fs->disk + 2 * IDKFS_BLOCK_SIZE, fs->inodes, sizeof(fs->inodes));
  fs->dirty = false;
}

static void idkfs_destroy(IDKFS *fs) {
  if (!fs)
    return;
  idkfs_flush(fs);
  free(fs->disk);
  free(fs);
}

static IDKFS *idkfs_mkfs(const char *label, FeatureFlags features) {
  IDKFS *fs = calloc(1, sizeof(IDKFS));
  if (!fs)
    return NULL;
  fs->disk_size = (size_t)IDKFS_TOTAL_BLOCKS * IDKFS_BLOCK_SIZE;
  fs->disk = calloc(1, fs->disk_size);
  if (!fs->disk) {
    free(fs);
    return NULL;
  }
  fs->features = features;
  uint32_t inode_blocks =
      (IDKFS_MAX_INODES * sizeof(Inode) + IDKFS_BLOCK_SIZE - 1) /
      IDKFS_BLOCK_SIZE;
  uint32_t data_start = 2 + inode_blocks;
  fs->sb = (Superblock){.magic = IDKFS_MAGIC,
                        .version = IDKFS_VERSION,
                        .block_size = IDKFS_BLOCK_SIZE,
                        .total_blocks = IDKFS_TOTAL_BLOCKS,
                        .free_blocks = IDKFS_TOTAL_BLOCKS - data_start,
                        .total_inodes = IDKFS_MAX_INODES,
                        .free_inodes = IDKFS_MAX_INODES - 1,
                        .inode_table_block = 2,
                        .bitmap_block = 1,
                        .data_start_block = data_start,
                        .root_inode = IDKFS_ROOT_INODE,
                        .created_at = (uint64_t)time(NULL),
                        .last_mounted = (uint64_t)time(NULL),
                        .mount_count = 1};
  strncpy(fs->sb.label, label, 63);
  for (uint32_t i = 0; i < data_start; i++)
    bitmap_set(&fs->bitmap, i);
  Inode *root = &fs->inodes[IDKFS_ROOT_INODE];
  root->ino = IDKFS_ROOT_INODE;
  root->type = IDKFS_FT_DIR;
  root->tier = TIER_NORMAL;
  root->mode = 0755;
  root->link_count = 2;
  root->atime = root->mtime = root->ctime = (uint64_t)time(NULL);
  for (int i = 0; i < IDKFS_DIRECT_BLOCKS; i++)
    root->direct[i] = IDKFS_NULL_BLOCK;
  root->indirect1 = root->indirect2 = IDKFS_NULL_BLOCK;
  idkfs_dir_add(fs, root, ".", IDKFS_ROOT_INODE, IDKFS_FT_DIR);
  idkfs_dir_add(fs, root, "..", IDKFS_ROOT_INODE, IDKFS_FT_DIR);
  if (features.checksums)
    fs->sb.checksum = crc32(&fs->sb, sizeof(Superblock) - sizeof(uint32_t));
  return fs;
}
