#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define open _open
#define write _write
#define close _close
#ifndef O_BINARY
#define O_BINARY 0
#endif
#define cpu_to_le32(x) ((uint32_t)(x))
#define cpu_to_le64(x) ((uint64_t)(x))
#else
#include <unistd.h>
#include <fcntl.h>
#include <endian.h>
#ifndef cpu_to_le32
#define cpu_to_le32(x) htole32((x))
#endif
#ifndef cpu_to_le64
#define cpu_to_le64(x) htole64((x))
#endif
#endif
#include "idkfs_kfs.h"
struct idkfs_tx_record tx;
struct idkfs_snapshot_table snaps;
int main(int argc, char *argv[]) {
	if (argc < 2) return 1;
	int fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
	if (fd < 0) return 1;
	struct idkfs_super_block sb;
	memset(&sb, 0, sizeof(sb));
	sb.magic = cpu_to_le32(IDKFS_MAGIC);
	sb.version = cpu_to_le32(IDKFS_VERSION);
	sb.generation = cpu_to_le64(1);
	sb.total_blocks = cpu_to_le64(1024 * 1024ULL);
	sb.tx_region_block = cpu_to_le64(1);
	sb.snap_region_block = cpu_to_le64(2);
	sb.free_blocks = cpu_to_le64((1024 * 1024ULL) - 16);
	sb.next_fast = cpu_to_le64(10);
	sb.next_normal = cpu_to_le64((1024 * 1024ULL) / 4);
	sb.next_slow = cpu_to_le64((1024 * 1024ULL) / 2);
	if (write(fd, &sb, sizeof(sb)) != sizeof(sb)) {
		close(fd);
		return 1;
	}
	memset(&tx, 0, sizeof(tx));
	tx.magic = cpu_to_le32(IDKFS_TX_MAGIC);
	tx.state = cpu_to_le32(0);
	tx.pending_generation = cpu_to_le64(1);
	tx.committed_generation = cpu_to_le64(1);
	if (write(fd, &tx, sizeof(tx)) != sizeof(tx)) {
		close(fd);
		return 1;
	}
	memset(&snaps, 0, sizeof(snaps));
	snaps.magic = cpu_to_le32(IDKFS_SNAP_MAGIC);
	snaps.next_id = cpu_to_le32(1);
	if (write(fd, &snaps, sizeof(snaps)) != sizeof(snaps)) {
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}
