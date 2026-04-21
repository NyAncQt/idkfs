#!/bin/sh
set -eu

if [ "$#" -lt 3 ]; then
  echo "usage: $0 <idkfs_mount> <ext4_mount> <btrfs_mount>"
  exit 1
fi

IDKFS_MNT="$1"
EXT4_MNT="$2"
BTRFS_MNT="$3"
OUT_DIR="${OUT_DIR:-./bench/results}"
RUNTIME="${RUNTIME:-20}"
SIZE="${SIZE:-1G}"

mkdir -p "$OUT_DIR"

run_case() {
  fs_name="$1"
  mount_path="$2"
  test_name="$3"
  rw="$4"
  bs="$5"
  iodepth="$6"
  numjobs="$7"

  fio --name="$test_name" \
      --directory="$mount_path" \
      --filename=".fio_${test_name}.dat" \
      --rw="$rw" \
      --ioengine=libaio \
      --direct=1 \
      --bs="$bs" \
      --iodepth="$iodepth" \
      --numjobs="$numjobs" \
      --time_based=1 \
      --runtime="$RUNTIME" \
      --size="$SIZE" \
      --group_reporting=1 \
      --output="$OUT_DIR/${fs_name}_${test_name}.txt"
}

for fs in "idkfs:$IDKFS_MNT" "ext4:$EXT4_MNT" "btrfs:$BTRFS_MNT"; do
  fs_name="${fs%%:*}"
  fs_path="${fs##*:}"
  run_case "$fs_name" "$fs_path" "seqwrite" "write" "1M" "8" "1"
  run_case "$fs_name" "$fs_path" "seqread" "read" "1M" "8" "1"
  run_case "$fs_name" "$fs_path" "randwrite" "randwrite" "4k" "32" "4"
  run_case "$fs_name" "$fs_path" "randread" "randread" "4k" "32" "4"
  run_case "$fs_name" "$fs_path" "mixed" "randrw" "4k" "32" "4"
done

echo "fio matrix complete. Results in $OUT_DIR"
