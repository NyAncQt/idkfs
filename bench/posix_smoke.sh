#!/bin/sh
set -eu

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <mount_path>"
  exit 1
fi

MNT="$1"
T="$MNT/.idkfs_posix_smoke"
rm -rf "$T"
mkdir -p "$T"

echo "hello" > "$T/a.txt"
cp "$T/a.txt" "$T/b.txt"
mv "$T/b.txt" "$T/c.txt"
mkdir -p "$T/dir1"
echo "x" > "$T/dir1/file1"
mv "$T/dir1/file1" "$T/dir1/file2"
rm -f "$T/c.txt"
rmdir "$T/dir1" 2>/dev/null || true
rm -f "$T/dir1/file2" || true
rmdir "$T/dir1"
sync

echo "posix smoke passed"
