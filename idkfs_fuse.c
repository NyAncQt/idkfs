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
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <pthread.h>
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

typedef struct {
  char name[IDKFS_MAX_FILENAME + 1];
  const char *type_name;
  uint64_t size;
  uint32_t inode;
  uint8_t entry_type;
} SortCandidate;

static lua_State *g_sort_L = NULL;
static int g_sort_compare_ref = LUA_REFNIL;
static bool g_sort_ready = false;
static pthread_mutex_t g_sort_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *entry_type_name(uint8_t type) {
  switch (type) {
  case IDKFS_FT_DIR:
    return "dir";
  case IDKFS_FT_FILE:
    return "file";
  case IDKFS_FT_SYMLINK:
    return "symlink";
  case IDKFS_FT_DEVICE:
    return "device";
  default:
    return "unknown";
  }
}

static void push_candidate_to_lua(lua_State *L, const SortCandidate *entry) {
  lua_newtable(L);
  lua_pushstring(L, entry->name);
  lua_setfield(L, -2, "name");
  lua_pushinteger(L, (lua_Integer)entry->inode);
  lua_setfield(L, -2, "inode");
  lua_pushinteger(L, (lua_Integer)entry->size);
  lua_setfield(L, -2, "size");
  lua_pushstring(L, entry->type_name);
  lua_setfield(L, -2, "type");
}

static int lua_sort_compare(const void *a, const void *b) {
  if (!g_sort_ready || !g_sort_L)
    return 0;

  const SortCandidate *left = (const SortCandidate *)a;
  const SortCandidate *right = (const SortCandidate *)b;

  pthread_mutex_lock(&g_sort_mutex);
  lua_rawgeti(g_sort_L, LUA_REGISTRYINDEX, g_sort_compare_ref);
  push_candidate_to_lua(g_sort_L, left);
  push_candidate_to_lua(g_sort_L, right);
  int result = 0;
  if (lua_pcall(g_sort_L, 2, 1, 0) != LUA_OK) {
    fprintf(stderr, "idkfs: lua compare error: %s\n", lua_tostring(g_sort_L, -1));
    lua_pop(g_sort_L, 1);
    g_sort_ready = false;
  } else {
    if (lua_isinteger(g_sort_L, -1))
      result = (int)lua_tointeger(g_sort_L, -1);
    lua_pop(g_sort_L, 1);
  }
  pthread_mutex_unlock(&g_sort_mutex);

  return result;
}

static void sorting_shutdown(void) {
  if (!g_sort_L)
    return;
  if (g_sort_compare_ref != LUA_REFNIL)
    luaL_unref(g_sort_L, LUA_REGISTRYINDEX, g_sort_compare_ref);
  lua_close(g_sort_L);
  g_sort_L = NULL;
  g_sort_ready = false;
  g_sort_compare_ref = LUA_REFNIL;
}

static void sorting_log_error(lua_State *L, const char *context) {
  const char *msg = lua_tostring(L, -1);
  fprintf(stderr, "idkfs: sorting lua %s: %s\n", context,
          msg ? msg : "unknown error");
  lua_pop(L, 1);
}

static bool sorting_init(void) {
  if (g_sort_ready)
    return true;

  const char *path = getenv("IDKFS_SORT_SCRIPT");
  if (!path)
    path = "sorting.lua";

  g_sort_L = luaL_newstate();
  if (!g_sort_L) {
    fprintf(stderr, "idkfs: failed to initialize Lua for sorting\n");
    return false;
  }

  luaL_openlibs(g_sort_L);
  if (luaL_loadfile(g_sort_L, path) != LUA_OK) {
    sorting_log_error(g_sort_L, "load failed");
    sorting_shutdown();
    return false;
  }
  if (lua_pcall(g_sort_L, 0, 0, 0) != LUA_OK) {
    sorting_log_error(g_sort_L, "execution failed");
    sorting_shutdown();
    return false;
  }

  lua_getglobal(g_sort_L, "compare");
  if (!lua_isfunction(g_sort_L, -1)) {
    fprintf(stderr, "idkfs: sorting.lua must export a 'compare(a,b)' function\n");
    sorting_shutdown();
    return false;
  }

  g_sort_compare_ref = luaL_ref(g_sort_L, LUA_REGISTRYINDEX);
  g_sort_ready = true;
  printf("idkfs: sorting enabled via '%s'\n", path);
  return true;
}

/* ============================================================
 * PATH RESOLUTION — walk path components to find inode
 * ============================================================ */

static int32_t resolve_path(const char *path) {
  if (strcmp(path, "/") == 0)
    return IDKFS_ROOT_INODE;

  char tmp[4096];
  strncpy(tmp, path, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

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
  tmp[sizeof(tmp) - 1] = '\0';

  char *slash = strrchr(tmp, '/');
  if (!slash)
    return -ENOENT;

  strncpy(name_out, slash + 1, IDKFS_MAX_FILENAME);
  name_out[IDKFS_MAX_FILENAME] = '\0';

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

  SortCandidate entries[IDKFS_MAX_DIRENTS];
  int entry_count = 0;

  for (uint32_t i = 0; i < dir.count && entry_count < IDKFS_MAX_DIRENTS; i++) {
    DirEntry *e = &dir.entries[i];
    if (strncmp(e->name, ".", IDKFS_MAX_FILENAME) == 0 ||
        strncmp(e->name, "..", IDKFS_MAX_FILENAME) == 0)
      continue;

    SortCandidate *candidate = &entries[entry_count++];
    memcpy(candidate->name, e->name, e->name_len);
    candidate->name[e->name_len] = '\0';
    candidate->inode = e->inode;
    candidate->entry_type = e->type;
    candidate->type_name = entry_type_name(e->type);
    candidate->size = gfs->inodes[e->inode].size;
  }

  if (entry_count > 1 && g_sort_ready)
    qsort(entries, entry_count, sizeof(entries[0]), lua_sort_compare);

  for (int i = 0; i < entry_count; i++) {
    SortCandidate *candidate = &entries[i];
    struct stat st = {0};
    st.st_ino = candidate->inode;
    if (candidate->entry_type == IDKFS_FT_DIR)
      st.st_mode = S_IFDIR | 0755;
    else if (candidate->entry_type == IDKFS_FT_SYMLINK)
      st.st_mode = S_IFLNK | 0777;
    else
      st.st_mode = S_IFREG | 0644;
    st.st_size = (off_t)candidate->size;
    filler(buf, candidate->name, &st, 0, 0);
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
  uint32_t parent;
  char name[IDKFS_MAX_FILENAME + 1];
  int ret = resolve_parent(path, &parent, name);
  if (ret < 0)
    return ret;

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

  ret = idkfs_dir_remove(gfs, &gfs->inodes[parent], name);
  if (ret < 0)
    return ret;

  inode_free(gfs, (uint32_t)ino);
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

  if (gfs->features.sorting)
    sorting_init();

  printf("idkfs: mounting via FUSE...\n");
  int ret = fuse_main(argc, argv, &idkfs_ops, NULL);

  sorting_shutdown();

  idkfs_destroy(gfs);
  return ret;
}
