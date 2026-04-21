#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#define open _open
#define read _read
#define close _close
#ifndef O_BINARY
#define O_BINARY 0
#endif
#define le32toh(x) ((uint32_t)(x))
#define le64toh(x) ((uint64_t)(x))
#else
#include <unistd.h>
#include <endian.h>
#endif
#include "../idkfs_kfs.h"

static int read_super(int fd, struct idkfs_super_block *sb) {
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    int got = read(fd, sb, (unsigned int)sizeof(*sb));
    return got == (int)sizeof(*sb) ? 0 : -1;
}
static int read_tx(int fd, const struct idkfs_super_block *sb, struct idkfs_tx_record *tx) {
    if (lseek(fd, (long)(le64toh(sb->tx_region_block) * IDKFS_BLOCK_SIZE), SEEK_SET) < 0)
        return -1;
    int got = read(fd, tx, (unsigned int)sizeof(*tx));
    return got == (int)sizeof(*tx) ? 0 : -1;
}
static int read_snaps(int fd, const struct idkfs_super_block *sb, struct idkfs_snapshot_table *snaps) {
    if (lseek(fd, (long)(le64toh(sb->snap_region_block) * IDKFS_BLOCK_SIZE), SEEK_SET) < 0)
        return -1;
    int got = read(fd, snaps, (unsigned int)sizeof(*snaps));
    return got == (int)sizeof(*snaps) ? 0 : -1;
}

int main(int argc, char **argv) {
    struct idkfs_super_block sb;
    uint64_t total;
    uint64_t free_blocks;
    uint64_t next_fast;
    uint64_t next_normal;
    uint64_t next_slow;
    int fd;
    struct idkfs_tx_record tx;
    struct idkfs_snapshot_table snaps;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <idkfs_image>\n", argv[0]);
        return 2;
    }

    fd = open(argv[1], O_RDONLY | O_BINARY);
    if (fd < 0) {
        perror("open");
        return 2;
    }

    if (read_super(fd, &sb) != 0) {
        perror("read superblock");
        close(fd);
        return 2;
    }
    close(fd);

    if (le32toh(sb.magic) != IDKFS_MAGIC) {
        fprintf(stderr, "idkfs_fsck: bad magic\n");
        return 1;
    }
    if (le32toh(sb.version) < 1 || le32toh(sb.version) > IDKFS_VERSION) {
        fprintf(stderr, "idkfs_fsck: unsupported version\n");
        return 1;
    }

    total = le64toh(sb.total_blocks);
    free_blocks = le64toh(sb.free_blocks);
    next_fast = le64toh(sb.next_fast);
    next_normal = le64toh(sb.next_normal);
    next_slow = le64toh(sb.next_slow);

    if (free_blocks > total) {
        fprintf(stderr, "idkfs_fsck: free_blocks > total_blocks\n");
        return 1;
    }
    if (!(next_fast <= next_normal && next_normal <= next_slow && next_slow <= total)) {
        fprintf(stderr, "idkfs_fsck: tier pointers invalid\n");
        return 1;
    }
    fd = open(argv[1], O_RDONLY | O_BINARY);
    if (fd < 0) {
        perror("open");
        return 2;
    }
    if (read_tx(fd, &sb, &tx) != 0) {
        perror("read tx");
        close(fd);
        return 2;
    }
    if (read_snaps(fd, &sb, &snaps) != 0) {
        perror("read snapshots");
        close(fd);
        return 2;
    }
    close(fd);
    if (le32toh(tx.magic) != IDKFS_TX_MAGIC) {
        fprintf(stderr, "idkfs_fsck: tx region invalid\n");
        return 1;
    }
    if (le32toh(snaps.magic) != IDKFS_SNAP_MAGIC) {
        fprintf(stderr, "idkfs_fsck: snapshot region invalid\n");
        return 1;
    }
    if (le32toh(snaps.count) > IDKFS_MAX_SNAPSHOTS) {
        fprintf(stderr, "idkfs_fsck: snapshot count invalid\n");
        return 1;
    }

    printf("idkfs_fsck: clean (basic superblock checks passed)\n");
    return 0;
}
