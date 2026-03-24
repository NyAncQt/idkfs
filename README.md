# idkfs — I Don't Know Filesystem

A fast, configurable FUSE filesystem written in C.

## stack
- **C** — core: superblock, inodes, block allocator, VFS
- **Rust** — daemon + cross-platform layer (coming)
- **JS** — user config via `idkfs.config.js` (coming)
- **Lua** — sorting algorithms + runtime hooks (coming)

## build
```bash
gcc -O2 -Wall -Wextra $(pkg-config --cflags --libs fuse3) -o idkfs_fuse idkfs_fuse.c
```

## usage
```bash
mkdir ~/idkfs_mount
./idkfs_fuse ~/idkfs_mount
# unmount
fusermount3 -u ~/idkfs_mount
```

## features
- speed tiers per file (FAST / NORMAL / SLOW)
- toggleable journaling, checksums, timestamps, compression
- configurable via JS (coming)
- Lua sorting hooks (coming)
