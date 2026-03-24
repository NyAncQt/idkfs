/*
 * idkfs_persist.c — persistence layer for idkfs
 * replaces RAM buffer with a real file on disk via mmap
 *
 * Compile:
 *   gcc -O2 -Wall -Wextra $(pkg-config --cflags --libs fuse3) -o idkfs
 * idkfs_persist.c
 *
 * Usage:
 *   # create a new 256MB filesystem image
 *   ./idkfs --mkfs idkfs.img
 *
 *   # mount an existing image
 *   mkdir -p ~/idkfs_mount
 *   ./idkfs --image idkfs.img ~/idkfs_mount
 *
 *   # unmount
 *   fusermount3 -u ~/idkfs_mount
 *
 * The image file is a flat binary — the entire fs lives in it.
 * mmap keeps it synced to disk automatically.
 */

#define FUSE_USE_VERSION 31
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* pull in core (no main) */
#include "idkfs_core.c"

/* ============================================================
 * DISK IMAGE — mmap based persistence
 * ============================================================ */

#define IDKFS_IMAGE_SIZE                                                       \
  ((size_t)IDKFS_TOTAL_BLOCKS * IDKFS_BLOCK_SIZE) /* 256MB */

typedef struct {
  int fd;
  char path[4096];
  uint8_t *map; /* mmap pointer */
  size_t size;
} DiskImage;

static DiskImage g_image = {0};
static IDKFS *gfs = NULL;

/* create a new blank image file */
static int image_create(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror("open");
    return -1;
  }

  /* extend file to full size */
  if (ftruncate(fd, (off_t)IDKFS_IMAGE_SIZE) < 0) {
    perror("ftruncate");
    close(fd);
    return -1;
  }

  printf("idkfs: created image '%s' (%.0f MB)\n", path,
         (double)IDKFS_IMAGE_SIZE / (1024 * 1024));
  close(fd);
  return 0;
}

/* open and mmap an existing image */
static int image_open(DiskImage *img, const char *path) {
  img->fd = open(path, O_RDWR);
  if (img->fd < 0) {
    perror("open");
    return -1;
  }

  struct stat st;
  if (fstat(img->fd, &st) < 0) {
    perror("fstat");
    close(img->fd);
    return -1;
  }
  img->size = (size_t)st.st_size;

  if (img->size < IDKFS_IMAGE_SIZE) {
    fprintf(stderr, "idkfs: image too small (%zu < %zu)\n", img->size,
            IDKFS_IMAGE_SIZE);
    close(img->fd);
    return -1;
  }

  img->map =
      mmap(NULL, img->size, PROT_READ | PROT_WRITE, MAP_SHARED, img->fd, 0);
  if (img->map == MAP_FAILED) {
    perror("mmap");
    close(img->fd);
    return -1;
  }

    strncpy(img->path, path, sizeof(img->path)-1);
    printf("idkfs: opened image '%s' (%.0f MB)\n",
           path, (double)img->size / (1024*1024));
    return 0;
}

/* sync mmap to disk explicitly */
static void image_sync(DiskImage *img) {
    if (!img->map) return;
    msync(img->map, img->size, MS_ASYNC);
}

/* close and unmap */
static void image_close(DiskImage *img) {
    if (!img->map) return;
    msync(img->map, img->size, MS_SYNC);
    munmap(img->map, img->size);
    close(img->fd);
    img->map = NULL;
    img->fd  = -1;
    printf("idkfs: image synced and closed\n");
}

/* ============================================================
 * IDKFS INIT WITH MMAP BACKING
 * instead of calloc, point fs->disk at the mmap region
 * ============================================================ */

static IDKFS *idkfs_mount_image(DiskImage *img, bool is_new) {
    IDKFS *fs = calloc(1, sizeof(IDKFS));
    if (!fs) return NULL;

    /* point disk buffer at mmap — no copy, writes go straight to file */
    fs->disk      = img->map;
    fs->disk_size = img->size;
    fs->features  = features_default();
    fs->dirty     = false;

    if (is_new) {
        /* format fresh filesystem */
        uint32_t inode_blocks = (IDKFS_MAX_INODES * sizeof(Inode) + IDKFS_BLOCK_SIZE - 1) / IDKFS_BLOCK_SIZE;
        uint32_t data_start   = 2 + inode_blocks;

        fs->sb = (Superblock){
            .magic=IDKFS_MAGIC, .version=IDKFS_VERSION, .block_size=IDKFS_BLOCK_SIZE,
            .total_blocks=IDKFS_TOTAL_BLOCKS, .free_blocks=IDKFS_TOTAL_BLOCKS-data_start,
            .total_inodes=IDKFS_MAX_INODES, .free_inodes=IDKFS_MAX_INODES-1,
            .inode_table_block=2, .bitmap_block=1, .data_start_block=data_start,
            .root_inode=IDKFS_ROOT_INODE,
            .created_at=(uint64_t)time(NULL), .last_mounted=(uint64_t)time(NULL), .mount_count=1,
        };
        strncpy(fs->sb.label, "idkfs", 63);

        for (uint32_t i = 0; i < data_start; i++) bitmap_set(&fs->bitmap, i);

        Inode *root = &fs->inodes[IDKFS_ROOT_INODE];
        root->ino=IDKFS_ROOT_INODE; root->type=IDKFS_FT_DIR; root->tier=TIER_NORMAL;
        root->mode=0755; root->link_count=2;
        root->atime=root->mtime=root->ctime=(uint64_t)time(NULL);
        for (int i=0; i<IDKFS_DIRECT_BLOCKS; i++) root->direct[i]=IDKFS_NULL_BLOCK;
        root->indirect1=root->indirect2=IDKFS_NULL_BLOCK;

        idkfs_dir_add(fs, root, ".",  IDKFS_ROOT_INODE, IDKFS_FT_DIR);
        idkfs_dir_add(fs, root, "..", IDKFS_ROOT_INODE, IDKFS_FT_DIR);

        /* flush in-memory structs to mmap */
        idkfs_flush(fs);
        image_sync(img);
        printf("idkfs: formatted new filesystem\n");
    } else {
        /* load existing fs from mmap */
        Superblock *sb_on_disk = (Superblock *)img->map;
        if (sb_on_disk->magic != IDKFS_MAGIC) {
            fprintf(stderr, "idkfs: bad magic — not an idkfs image (got 0x%08X)\n", sb_on_disk->magic);
            free(fs); return NULL;
        }

        /* read superblock, bitmap, inodes from disk into fs structs */
        memcpy(&fs->sb,     img->map,                          sizeof(Superblock));
        memcpy(&fs->bitmap, img->map + IDKFS_BLOCK_SIZE,       sizeof(BlockBitmap));
        memcpy(fs->inodes,  img->map + 2*IDKFS_BLOCK_SIZE,     sizeof(fs->inodes));

        fs->sb.last_mounted = (uint64_t)time(NULL);
        fs->sb.mount_count++;
        printf("idkfs: loaded filesystem (mount #%u, %u free blocks, %u free inodes)\n",
               fs->sb.mount_count, fs->sb.free_blocks, fs->sb.free_inodes);
    }

    return fs;
}

/* flush in-memory fs state back to mmap (and thus to disk) */
static void idkfs_flush_to_image(IDKFS *fs, DiskImage *img) {
    memcpy(img->map,                      &fs->sb,     sizeof(Superblock));
    memcpy(img->map + IDKFS_BLOCK_SIZE,   &fs->bitmap, sizeof(BlockBitmap));
    memcpy(img->map + 2*IDKFS_BLOCK_SIZE, fs->inodes,  sizeof(fs->inodes));
    image_sync(img);
}

/* ============================================================
 * PATH RESOLUTION
 * ============================================================ */

static int32_t resolve_path(const char *path) {
    if (strcmp(path, "/") == 0) return IDKFS_ROOT_INODE;
    char tmp[4096]; strncpy(tmp, path, sizeof(tmp)-1);
    uint32_t cur = IDKFS_ROOT_INODE;
    char *tok = strtok(tmp, "/");
    while (tok) {
        Inode *dir = &gfs->inodes[cur];
        if (dir->type != IDKFS_FT_DIR) return -ENOTDIR;
        int32_t next = idkfs_dir_lookup(gfs, dir, tok);
        if (next < 0) return -ENOENT;
        cur = (uint32_t)next;
        tok = strtok(NULL, "/");
    }
    return (int32_t)cur;
}

static int resolve_parent(const char *path, uint32_t *parent_out, char *name_out) {
    char tmp[4096]; strncpy(tmp, path, sizeof(tmp)-1);
    char *slash = strrchr(tmp, '/');
    if (!slash) return -ENOENT;
    strncpy(name_out, slash+1, IDKFS_MAX_FILENAME);
    if (slash == tmp) { *parent_out = IDKFS_ROOT_INODE; }
    else {
        *slash = '\0';
        int32_t p = resolve_path(tmp);
        if (p < 0) return p;
        *parent_out = (uint32_t)p;
    }
    return 0;
}

/* ============================================================
 * FUSE CALLBACKS
 * ============================================================ */

static int fuse_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    int32_t ino = resolve_path(path);
    if (ino < 0) return ino;
    Inode *i = &gfs->inodes[ino];
    memset(st, 0, sizeof(*st));
    st->st_ino    = i->ino;
    st->st_mode   = i->mode;
    st->st_nlink  = i->link_count;
    st->st_uid    = i->uid; st->st_gid = i->gid;
    st->st_size   = (off_t)i->size;
    st->st_atime  = (time_t)i->atime;
    st->st_mtime  = (time_t)i->mtime;
    st->st_ctime  = (time_t)i->ctime;
    st->st_blksize = IDKFS_BLOCK_SIZE;
    st->st_blocks  = i->block_count * (IDKFS_BLOCK_SIZE/512);
    if      (i->type==IDKFS_FT_DIR)     st->st_mode |= S_IFDIR;
    else if (i->type==IDKFS_FT_FILE)    st->st_mode |= S_IFREG;
    else if (i->type==IDKFS_FT_SYMLINK) st->st_mode |= S_IFLNK;
    return 0;
}

static int fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    int32_t ino = resolve_path(path);
    if (ino < 0) return ino;
    Inode *dir_ino = &gfs->inodes[ino];
    if (dir_ino->type != IDKFS_FT_DIR) return -ENOTDIR;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    if (dir_ino->size == 0) return 0;
    Directory dir; idkfs_read(gfs, dir_ino, &dir, sizeof(dir), 0);
    for (uint32_t i = 0; i < dir.count; i++) {
        DirEntry *e = &dir.entries[i];
        if (strncmp(e->name,".", IDKFS_MAX_FILENAME)==0) continue;
        if (strncmp(e->name,"..",IDKFS_MAX_FILENAME)==0) continue;
        char name[IDKFS_MAX_FILENAME+1];
        memcpy(name, e->name, e->name_len); name[e->name_len]='\0';
        struct stat st={0};
        st.st_ino  = e->inode;
        st.st_mode = (e->type==IDKFS_FT_DIR) ? S_IFDIR|0755 : S_IFREG|0644;
        filler(buf, name, &st, 0, 0);
    }
    return 0;
}

static int fuse_open(const char *path, struct fuse_file_info *fi) {
    int32_t ino = resolve_path(path);
    if (ino < 0) return ino;
    fi->fh = (uint64_t)ino;
    return 0;
}

static int fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void)path;
    uint32_t ino = (uint32_t)fi->fh;
    if (ino >= IDKFS_MAX_INODES) return -EBADF;
    int n = (int)idkfs_read(gfs, &gfs->inodes[ino], buf, size, (uint64_t)offset);
    idkfs_flush_to_image(gfs, &g_image);
    return n;
}

static int fuse_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void)path;
    uint32_t ino = (uint32_t)fi->fh;
    if (ino >= IDKFS_MAX_INODES) return -EBADF;
    ssize_t n = idkfs_write(gfs, &gfs->inodes[ino], buf, size, (uint64_t)offset);
    idkfs_flush_to_image(gfs, &g_image);
    return (int)n;
}

static int fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    uint32_t parent; char name[IDKFS_MAX_FILENAME+1];
    int ret = resolve_parent(path, &parent, name);
    if (ret < 0) return ret;
    int32_t ino = idkfs_create(gfs, parent, name, TIER_NORMAL, (uint16_t)mode);
    if (ino < 0) return -ENOSPC;
    fi->fh = (uint64_t)ino;
    idkfs_flush_to_image(gfs, &g_image);
    return 0;
}

static int fuse_mkdir(const char *path, mode_t mode) {
    uint32_t parent; char name[IDKFS_MAX_FILENAME+1];
    int ret = resolve_parent(path, &parent, name);
    if (ret < 0) return ret;
    int32_t ino = idkfs_mkdir(gfs, parent, name, (uint16_t)mode);
    if (ino < 0) return -ENOSPC;
    idkfs_flush_to_image(gfs, &g_image);
    return 0;
}

static int fuse_unlink(const char *path) {
    int32_t ino = resolve_path(path);
    if (ino < 0) return ino;
    Inode *i = &gfs->inodes[ino];
    if (i->type == IDKFS_FT_DIR) return -EISDIR;
    for (int b=0; b<IDKFS_DIRECT_BLOCKS; b++) {
        if (i->direct[b] != IDKFS_NULL_BLOCK) {
            bitmap_clear(&gfs->bitmap, i->direct[b]);
            gfs->sb.free_blocks++;
            i->direct[b] = IDKFS_NULL_BLOCK;
        }
    }
    inode_free(gfs, (uint32_t)ino);
    idkfs_flush_to_image(gfs, &g_image);
    return 0;
}

static int fuse_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    int32_t ino = resolve_path(path);
    if (ino < 0) return ino;
    gfs->inodes[ino].size = (uint64_t)size;
    idkfs_flush_to_image(gfs, &g_image);
    return 0;
}

static int fuse_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi) {
    (void)fi;
    if (!gfs->features.timestamps) return 0;
    int32_t ino = resolve_path(path);
    if (ino < 0) return ino;
    gfs->inodes[ino].atime = (uint64_t)tv[0].tv_sec;
    gfs->inodes[ino].mtime = (uint64_t)tv[1].tv_sec;
    idkfs_flush_to_image(gfs, &g_image);
    return 0;
}

static int fuse_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    int32_t ino = resolve_path(path);
    if (ino < 0) return ino;
    gfs->inodes[ino].mode = (uint16_t)mode;
    idkfs_flush_to_image(gfs, &g_image);
    return 0;
}

static int fuse_statfs(const char *path, struct statvfs *st) {
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize  = IDKFS_BLOCK_SIZE; st->f_frsize = IDKFS_BLOCK_SIZE;
    st->f_blocks = IDKFS_TOTAL_BLOCKS; st->f_bfree = gfs->sb.free_blocks;
    st->f_bavail = gfs->sb.free_blocks; st->f_files = IDKFS_MAX_INODES;
    st->f_ffree  = gfs->sb.free_inodes; st->f_namemax = IDKFS_MAX_FILENAME;
    return 0;
}

static void idkfs_fuse_destroy(void *private_data) {
    (void)private_data;
    if (gfs) { idkfs_flush_to_image(gfs, &g_image); free(gfs); gfs=NULL; }
    image_close(&g_image);
}

static const struct fuse_operations idkfs_ops = {
    .getattr  = fuse_getattr,
    .readdir  = fuse_readdir,
    .open     = fuse_open,
    .read     = fuse_read,
    .write    = fuse_write,
    .create   = fuse_create,
    .mkdir    = fuse_mkdir,
    .unlink   = fuse_unlink,
    .truncate = fuse_truncate,
    .utimens  = fuse_utimens,
    .chmod    = fuse_chmod,
    .statfs   = fuse_statfs,
    .destroy  = idkfs_fuse_destroy,
};

/* ============================================================
 * MAIN
 * ============================================================ */

static void usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  idkfs --mkfs <image>              create new filesystem image\n"
        "  idkfs --image <image> <mountpoint> [fuse opts]  mount image\n"
    );
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(); return 1; }

    /* --mkfs: just create the image and format it */
    if (strcmp(argv[1], "--mkfs") == 0) {
        if (argc < 3) { usage(); return 1; }
        if (image_create(argv[2]) < 0) return 1;
        if (image_open(&g_image, argv[2]) < 0) return 1;
        IDKFS *fs = idkfs_mount_image(&g_image, true);
        if (!fs) return 1;
        idkfs_flush_to_image(fs, &g_image);
        free(fs);
        image_close(&g_image);
        printf("idkfs: done. mount with: idkfs --image %s <mountpoint>\n", argv[2]);
        return 0;
    }

    /* --image <path> <mountpoint> [fuse opts] */
    if (strcmp(argv[1], "--image") == 0) {
        if (argc < 4) { usage(); return 1; }
        const char *image_path = argv[2];

        if (image_open(&g_image, image_path) < 0) return 1;

        /* check if valid idkfs image */
        Superblock *sb = (Superblock *)g_image.map;
        bool is_new = (sb->magic != IDKFS_MAGIC);
        if (is_new) printf("idkfs: no valid superblock found, formatting...\n");

        gfs = idkfs_mount_image(&g_image, is_new);
        if (!gfs) { image_close(&g_image); return 1; }

        /* rebuild fuse argv: argv[0] + mountpoint + any extra fuse opts */
        /* shift argv so fuse sees: [prog, mountpoint, opts...] */
        argv[2] = argv[0];
        return fuse_main(argc - 2, argv + 2, &idkfs_ops, NULL);
    }

    usage();
    return 1;
}
