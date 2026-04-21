#!/bin/sh
set -eu

if [ "$#" -lt 4 ]; then
  echo "usage: $0 <idkfs_mount> <ext4_mount> <btrfs_mount> <image_path>"
  exit 1
fi

IDKFS_MNT="$1"
EXT4_MNT="$2"
BTRFS_MNT="$3"
IMG="$4"

./bench/posix_smoke.sh "$IDKFS_MNT"
./bench/fault_injection_smoke.sh "$IDKFS_MNT" "$IMG"
./idkfs_fsck "$IMG"
./bench/run_fio_matrix.sh "$IDKFS_MNT" "$EXT4_MNT" "$BTRFS_MNT"

echo "canary gate sequence completed"
