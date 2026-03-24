---

````markdown
# IDKFS

IDKFS (I Don’t Know Filesystem) is a custom FUSE-based filesystem written in C, designed for experimentation and modular file management. It supports persistent storage, snapshots, Lua-powered sorting, and flexible configuration via JS and Lua modules. IDKFS is primarily a user-space filesystem, making it easy to test without kernel modifications.

---

## Features

- **FUSE-based**: Runs entirely in user space without kernel modules.  
- **Modular architecture**: Easily extend functionality via modules.  
- **Persistent storage**: Files and metadata are stored in a single image.  
- **Snapshots**: Supports lightweight snapshots for testing and recovery.  
- **Lua sorting**: Customize file listing and organization dynamically.  
- **User-configurable options**: Adjust caching, journaling, and permissions.  

---

## Status

- Stable for testing and experimentation.  
- Works on Linux x86_64 (CachyOS tested, Linux 6.19+).  
- `allow_other` requires `/etc/fuse.conf` to be configured.  
- **Not recommended as the primary filesystem on production machines yet.**  

---

## Installation

1. Clone the repository:

```bash
git clone https://github.com/NyAncQt/idkfs.git
cd idkfs
````

2. Build the project:

```bash
make
```

3. Create a test filesystem image:

```bash
mkdir ~/idkfs_test
truncate -s 50M ~/idkfs_test/myfs.img
```

---

## Usage

1. Create a mount point:

```bash
mkdir ~/idkfs_mount
```

2. Mount the filesystem via FUSE:

```bash
./idkfs_fuse -f -o allow_other ~/idkfs_test/myfs.img ~/idkfs_mount
```

* `-f` keeps it in the foreground (useful for debugging).
* `allow_other` lets all users access the filesystem (requires `/etc/fuse.conf`).

3. Perform basic file operations:

```bash
touch ~/idkfs_mount/testfile
echo "hello world" > ~/idkfs_mount/testfile
cat ~/idkfs_mount/testfile
# Output: hello world
```

4. Unmount the filesystem:

```bash
fusermount3 -uz ~/idkfs_mount
```

---

## Configuration

* `idkfs.config.js`: Runtime configuration (enable/disable journaling, caching, etc.).
* `sorting.lua`: Customize file ordering and display logic.
* `Makefile`: Build settings and optional kernel module compilation.

---

## Options

IDKFS supports the following FUSE and module-specific options:

```
Usage: ./idkfs [options] <mountpoint>

Options:
    -h, --help             Show help
    -V, --version          Show version
    -f                     Run in foreground (debug)
    -o allow_other         Allow other users to access
    -o auto_unmount        Unmount automatically on exit
    -o io_uring            Enable io_uring support
    -o subdir=DIR          Prepend this directory to all paths
    -o from_code=CHARSET   Input encoding for filenames
    -o to_code=CHARSET     Output encoding for filenames
```

Refer to `idkfs.config.js` and `sorting.lua` for more fine-grained behavior.

---

## Contributing

* Contributions welcome!
* Ensure FUSE mounts are tested on Linux x86_64.
* Include kernel version if testing kernel modules.
* Submit PRs with clear descriptions and relevant tests.

---

## License

IDKFS is released under the MIT License. See [LICENSE](LICENSE) for details.

---

## Notes

* Kernel headers are only required if building optional kernel modules.
* Images can be created using `truncate -s <size>` for quick testing.
* FUSE is mandatory for user-space operation.
* Avoid using IDKFS as the root filesystem in production until stable kernel support is added.

```

---

If you want, I can also **write a “Quick Start” cheat sheet section at the very top** that shows a 5-step setup for new users—it makes your GitHub README feel super professional and approachable.  

Do you want me to do that too?
```

