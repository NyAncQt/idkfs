# IDKFS — The Sovereign Filesystem

IDKFS (I Don't Know Filesystem) is a Linux kernel filesystem project focused on tiered placement, transactional updates, and competitive performance. The current codebase is in active migration from userspace FUSE components to a kernel-first VFS path.

##  Core Architecture

- **Shadow-Paging CoW**: Data is never overwritten. Every write transaction creates a new version of the metadata tree, culminating in an atomic superblock generation swap.
- **Merkle-Tree Integrity**: Every metadata node and data extent is protected by kernel-level CRC32C checksums, verified on every read cycle.
- **Physical Zonal Tiering**: IDKFS automatically partitions your storage into FAST, NORMAL, and SLOW zones.
    - **FAST**: Binaries, libraries (`.so`, `.bin`, `.exe`) — allocated to low-latency zones.
    - **NORMAL**: Source code, documents, logs.
    - **SLOW**: Archives, images (`.iso`, `.tar`, `.zip`) — allocated to high-capacity zones.
- **Extent-Based Storage**: O(log N) lookup efficiency and minimal metadata overhead for petabyte-scale volumes.

##  Build & Install

### 1. Requirements
- Linux Kernel Headers
- GCC / Make
- `libelf-dev` (for kernel modules)

### 2. Compilation (Linux host)
```bash
# Build the kernel module and mkfs tool
make
gcc mkfs_idkfs.c -o mkfs.idkfs
```

### 3. Deployment
```bash
# Prepare a 1GB image (or use a raw block device)
truncate -s 1G idkfs.img

# Format with IDKFS Zonal Tiering
./mkfs.idkfs idkfs.img

# Load kernel modules
sudo insmod idkfs_blk.ko backing_path=$PWD/idkfs.img
sudo insmod idkfs_vfs.ko

# Mount filesystem
sudo mkdir -p /mnt/idkfs
sudo mount -t idkfs idkfs.img /mnt/idkfs
```

### 4. Benchmark baseline
Run the fio harness to compare against ext4 and btrfs on the same host/device:
```bash
./bench/run_fio_matrix.sh /mnt/idkfs /mnt/ext4 /mnt/btrfs
```

### 5. Crash-recovery smoke gate
Build includes `idkfs_fsck` (basic superblock checks). Run the fault-injection preflight and validate image consistency:
```bash
./bench/fault_injection_smoke.sh /mnt/idkfs ./idkfs.img
./idkfs_fsck ./idkfs.img
```

### 6. POSIX canary smoke gate
Run a minimal daily-use semantics smoke test on the mounted filesystem:
```bash
./bench/posix_smoke.sh /mnt/idkfs
```

### 7. Full canary gate sequence
Run all safety-first canary checks together:
```bash
./bench/canary_gate.sh /mnt/idkfs /mnt/ext4 /mnt/btrfs ./idkfs.img
```

##  Efficiency Mandate

IDKFS is written in **Zero-Comment Logic-Dense C**. It prioritizes:
1. **Instruction Cache Locality**: Minimal branches in the IO fast-path.
2. **Lock-Free Scaling**: Uses RCU-style shadow paging and per-zone spinlocks.
3. **No Garbage**: No background "scrubbers" required; integrity is verified at the hardware-abstraction layer.

##  License
GPLv2 - The Sovereign Standard.
