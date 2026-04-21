#!/bin/sh
set -eu

if [ "$#" -lt 2 ]; then
  echo "usage: $0 <idkfs_mount> <image_path>"
  exit 1
fi

MOUNT_PATH="$1"
IMAGE_PATH="$2"
FSCK_BIN="${FSCK_BIN:-./idkfs_fsck}"

fio --name=crashprep \
    --directory="$MOUNT_PATH" \
    --filename=.fault_injection.dat \
    --rw=randwrite \
    --ioengine=libaio \
    --direct=1 \
    --bs=4k \
    --iodepth=32 \
    --numjobs=4 \
    --time_based=1 \
    --runtime=10 \
    --size=256M \
    --group_reporting=1 >/dev/null

echo "Simulate abrupt crash now (e.g. sysrq-trigger or forced poweroff in VM), then rerun:"
echo "  $FSCK_BIN $IMAGE_PATH"
