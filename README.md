# idkfs — I Don't Know Filesystem

A fast, configurable FUSE filesystem written in C.

## stack
- **C** — core: superblock, inodes, block allocator, VFS
- **Rust** — daemon + cross-platform layer (coming)
- **JS** — user config via `idkfs.config.js` (coming)
- **Lua** — sorting algorithms + runtime hooks (coming)

## build
```bash
gcc -O2 -Wall -Wextra $(pkg-config --cflags --libs fuse3) $(pkg-config --cflags --libs lua) -pthread -o idkfs_fuse idkfs_fuse.c
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
- Lua sorting hooks driven by `sorting.lua`

## sorting

`idkfs_fuse` reads `sorting.lua` from the mount directory (or whatever path is
specified in `IDKFS_SORT_SCRIPT`) and expects a `compare(a, b)` function. Each
table argument contains `name`, `inode`, `size`, and `type` entries (e.g.,
`"dir"`/`"file"`/`"symlink"`). The provided script sorts entries by size first,
then by type, and finally by a case-insensitive name compare so the ordering
remains deterministic.

## system integration

### Rust helpers

The Rust workspace now builds two helpers:

 - `idkfsd` supervises the FUSE process, keeps the mount point alive, exposes
   `/run/idkfsd.sock`, and manages `<image>.snapshots/` metadata (`list.txt`,
   `next_id`). Point it at your persistent image and desired mount:
   `idkfsd --image /var/lib/idkfs/myfs.img --mount /mnt/idkfs`.
   Pass any extra flags to the underlying FUSE binary via repeated `--fuse-arg`
   (for example `--fuse-arg --image --fuse-arg /var/lib/idkfs/myfs.img`).
 - `idkfsctl` talks to that socket and provides `create`, `list`, `delete`, and
   `rollback` commands. `idkfsctl --socket /run/idkfsd.sock create "daily"` keeps
   the snapshot log consistent with the daemon.

Build them with Cargo:

```bash
cargo build --release -p idkfsd
cargo build --release -p idkfsctl
```

### Snapper hooks

To integrate with Snapper, point its hook commands at `idkfsctl`:

```ini
[config]
CREATE_CMD="/usr/local/bin/idkfsctl --socket /run/idkfsd.sock create \"$SNAPPER_DESCRIPTION\""
LIST_CMD="/usr/local/bin/idkfsctl --socket /run/idkfsd.sock list"
DELETE_CMD="/usr/local/bin/idkfsctl --socket /run/idkfsd.sock delete $SNAPPER_NUMBER"
ROLLBACK_CMD="/usr/local/bin/idkfsctl --socket /run/idkfsd.sock rollback $SNAPPER_NUMBER"
```

Snapper still controls scheduling and cleanup, while the helper copies the
idkfs image to/from `<image>.snapshots/` (the daemon watches w/ `list.txt`).

### Services for systemd & OpenRC

Deploy the unit files from `deploy/`:

 - `deploy/idkfsd.service` sources `/etc/idkfsd.conf`, starts `idkfsd`, and
   restarts it automatically.
 - `deploy/idkfsd.openrc` is the equivalent OpenRC script; it `.`’s the same
   `/etc/idkfsd.conf`, ensures the mount point exists, and runs `idkfsd` under
   `supervise`.

Populate `/etc/idkfsd.conf` with:

```
IDKFS_IMAGE=/var/lib/idkfs/myfs.img
IDKFS_MOUNT=/mnt/idkfs
```

Then enable the desired service and let the daemon keep idkfs mounted without
touching kernel drivers.
