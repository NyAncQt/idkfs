/*
 * idkfs_fuse.c — FUSE wrapper for idkfs
 *
 * Compile:
 *   gcc -O2 -Wall -Wextra $(pkg-config --cflags --libs fuse3) -o idkfs_fuse
 * idkfs_fuse.c
 *
 * Usage:
 *   mkdir -p /mnt/idkfs
 *   ./idkfs_fuse /mnt/idkfs
 *   ./idkfs_fuse /mnt/idkfs -d   # debug mode, see every call
 *
 * Unmount:
 *   fusermount3 -u /mnt/idkfs
 */

#define FUSE_USE_VERSION 31

#include <assert.h>
#include <errno.h>
#include <fuse.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── pull in the entire core inline ────────────────────────── */
#include "idkfs_core.c"

/* ── global fs instance ─────────────────────────────────────── */
static IDKFS *gfs = NULL;

/* ============================================================
 * PATH RESOLUTION — walk path components to find inode
 * ============================================================ */

static int32_t resolve_path(const char *path) {
  if (strcmp(path, "/") == 0)
    return IDKFS_ROOT_INODE;

  char tmp[4096];
  strncpy(tmp, path, sizeof(tmp) - 1);

  uint32_t cur = IDKFS_ROOT_INODE;
  char *tok = strtok(tmp, "/");

  while (tok) {
    Inode *dir = &gfs->inodes[cur];
    if (dir->type != IDKFS_FT_DIR)
      return -ENOTDIR;

    int32_t next = idkfs_dir_lookup(gfs, dir, tok);
    if (next < 0)
      return -ENOENT;
    cur = (uint32_t)next;
    tok = strtok(NULL, "/");
  }

  return (int32_t)cur;
}

/* get parent dir inode + basename from path */
static int resolve_parent(const char *path, uint32_t *parent_out,
                          char *name_out) {
  char tmp[4096];
  strncpy(tmp, path, sizeof(tmp) - 1);

  char *slash = strrchr(tmp, '/');
  if (!slash)
    return -ENOENT;

  strncpy(name_out, slash + 1, IDKFS_MAX_FILENAME);

  if (slash == tmp) {
    *parent_out = IDKFS_ROOT_INODE;
  } else {
    *slash = '\0';
    int32_t p = resolve_path(tmp);
    if (p < 0)
      return p;
    *parent_out = (uint32_t)p;
  }
  return 0;
}

/* ============================================================
 * FUSE CALLBACKS
 * ============================================================ */

static int fuse_getattr(const char *path, struct stat *st,
                        struct fuse_file_info *fi) {
  (void)fi;
  int32_t ino = resolve_path(path);
  if (ino < 0)
    return ino;

  Inode *i = &gfs->inodes[ino];
  memset(st, 0, sizeof(*st));

  st->st_ino = i->ino;
  st->st_mode = i->mode;
  st->st_nlink = i->link_count;
  st->st_uid = i->uid;
  st->st_gid = i->gid;
  st->st_size = (off_t)i->size;
  st->st_atime = (time_t)i->atime;
  st->st_mtime = (time_t)i->mtime;
  st->st_ctime = (time_t)i->ctime;
  st->st_blksize = IDKFS_BLOCK_SIZE;
  st->st_blocks = i->block_count * (IDKFS_BLOCK_SIZE / 512);

  if (i->type == IDKFS_FT_DIR)
    st->st_mode |= S_IFDIR;
  else if (i->type == IDKFS_FT_FILE)
    st->st_mode |= S_IFREG;
  else if (i->type == IDKFS_FT_SYMLINK)
    st->st_mode |= S_IFLNK;

  return 0;
}

static int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
  (void)offset;
  (void)fi;
  (void)flags;

  int32_t ino = resolve_path(path);
  if (ino < 0)
    return ino;

  Inode *dir_ino = &gfs->inodes[ino];
  if (dir_ino->type != IDKFS_FT_DIR)
    return -ENOTDIR;

  filler(buf, ".", NULL, 0, 0);
  filler(buf, "..", NULL, 0, 0);

  if (dir_ino->size == 0)
    return 0;

  Directory dir;
  idkfs_read(gfs, dir_ino, &dir, sizeof(dir), 0);

  for (uint32_t i = 0; i < dir.count; i++) {
    DirEntry *e = &dir.entries[i];
    /* skip . and .. — already added */
    if (strncmp(e->name, ".", IDKFS_MAX_FILENAME) == 0)
      continue;
    if (strncmp(e->name, "..", IDKFS_MAX_FILENAME) == 0)
      continue;

    char name[IDKFS_MAX_FILENAME + 1];
    memcpy(name, e->name, e->name_len);
    name[e->name_len] = '\0';

    struct stat st = {0};
    st.st_ino = e->inode;
    st.st_mode = (e->type == IDKFS_FT_DIR) ? S_IFDIR | 0755 : S_IFREG | 0644;
    filler(buf, name, &st, 0, 0);
  }

  return 0;
}

static int fuse_open(const char *path, struct fuse_file_info *fi) {
  int32_t ino = resolve_path(path);
  if (ino < 0)
    return ino;
  fi->fh = (uint64_t)ino;
  return 0;
}

static int fuse_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
  (void)path;
  uint32_t ino = (uint32_t)fi->fh;
  if (ino >= IDKFS_MAX_INODES)
    return -EBADF;
  return (int)idkfs_read(gfs, &gfs->inodes[ino], buf, size, (uint64_t)offset);
}

static int fuse_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi) {
  (void)path;
  uint32_t ino = (uint32_t)fi->fh;
  if (ino >= IDKFS_MAX_INODES)
    return -EBADF;
  ssize_t n = idkfs_write(gfs, &gfs->inodes[ino], buf, size, (uint64_t)offset);
  return (int)n;
}

static int fuse_create(const char *path, mode_t mode,
                       struct fuse_file_info *fi) {
  uint32_t parent;
  char name[IDKFS_MAX_FILENAME + 1];
  int ret = resolve_parent(path, &parent, name);
  if (ret < 0)
    return ret;

  int32_t ino = idkfs_create(gfs, parent, name, TIER_NORMAL, (uint16_t)mode);
  if (ino < 0)
    return -ENOSPC;

  fi->fh = (uint64_t)ino;
  return 0;
}

static int fuse_mkdir(const char *path, mode_t mode) {
  uint32_t parent;
  char name[IDKFS_MAX_FILENAME + 1];
  int ret = resolve_parent(path, &parent, name);
  if (ret < 0)
    return ret;

  int32_t ino = idkfs_mkdir(gfs, parent, name, (uint16_t)mode);
  if (ino < 0)
    return -ENOSPC;
  return 0;
}

static int fuse_truncate(const char *path, off_t size,
                         struct fuse_file_info *fi) {
  (void)fi;
  int32_t ino = resolve_path(path);
  if (ino < 0)
    return ino;
  gfs->inodes[ino].size = (uint64_t)size;
  return 0;
}

static int fuse_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi) {
  (void)fi;
  if (!gfs->features.timestamps)
    return 0;
  int32_t ino = resolve_path(path);
  if (ino < 0)
    return ino;
  gfs->inodes[ino].atime = (uint64_t)tv[0].tv_sec;
  gfs->inodes[ino].mtime = (uint64_t)tv[1].tv_sec;
  return 0;
}

static int fuse_chmod(const char *path, mode_t mode,
                      struct fuse_file_info *fi) {
  (void)fi;
  int32_t ino = resolve_path(path);
  if (ino < 0)
    return ino;
  gfs->inodes[ino].mode = (uint16_t)mode;
  return 0;
}

static int fuse_unlink(const char *path) {
  int32_t ino = resolve_path(path);
  if (ino < 0)
    return ino;

  Inode *i = &gfs->inodes[ino];
  if (i->type == IDKFS_FT_DIR)
    return -EISDIR;

  /* free all blocks */
  for (int b = 0; b < IDKFS_DIRECT_BLOCKS; b++) {
    if (i->direct[b] != IDKFS_NULL_BLOCK) {
      bitmap_clear(&gfs->bitmap, i->direct[b]);
      gfs->sb.free_blocks++;
      i->direct[b] = IDKFS_NULL_BLOCK;
    }
  }

  inode_free(gfs, (uint32_t)ino);
  /* TODO: remove from parent directory — needs dir_remove() */
  return 0;
}

static int fuse_statfs(const char *path, struct statvfs *st) {
  (void)path;
  memset(st, 0, sizeof(*st));
  st->f_bsize = IDKFS_BLOCK_SIZE;
  st->f_frsize = IDKFS_BLOCK_SIZE;
  st->f_blocks = IDKFS_TOTAL_BLOCKS;
  st->f_bfree = gfs->sb.free_blocks;
  st->f_bavail = gfs->sb.free_blocks;
  st->f_files = IDKFS_MAX_INODES;
  st->f_ffree = gfs->sb.free_inodes;
  st->f_namemax = IDKFS_MAX_FILENAME;
  return 0;
}

/* ============================================================
 * FUSE OPS TABLE
 * ============================================================ */

static const struct fuse_operations idkfs_ops = {
    .getattr = fuse_getattr,
    .readdir = fuse_readdir,
    .open = fuse_open,
    .read = fuse_read,
    .write = fuse_write,
    .create = fuse_create,
    .mkdir = fuse_mkdir,
    .unlink = fuse_unlink,
    .truncate = fuse_truncate,
    .utimens = fuse_utimens,
    .chmod = fuse_chmod,
    .statfs = fuse_statfs,
};

/* ============================================================
 * MAIN
 * ============================================================ */

int main(int argc, char *argv[]) {
  printf("idkfs: initializing...\n");

  gfs = idkfs_mkfs("idkfs", features_default());
  if (!gfs) {
    fprintf(stderr, "idkfs: mkfs failed\n");
    return 1;
  }

  printf("idkfs: mounting via FUSE...\n");
  int ret = fuse_main(argc, argv, &idkfs_ops, NULL);

  idkfs_destroy(gfs);
  return ret;
}
