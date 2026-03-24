#!/bin/sh
# install.sh — set up idkfsd, snapper hooks, and dependencies
set -eu

BASE="$(cd "$(dirname "$0")" && pwd)"
SERVICE_SRC="${BASE}/deploy/idkfsd.service"
OPENRC_SRC="${BASE}/deploy/idkfsd.openrc"
SERVICE_DST="/etc/systemd/system/idkfsd.service"
OPENRC_DST="/etc/init.d/idkfsd"
CONF="/etc/idkfsd.conf"
BIN_DIR="/usr/local/bin"
SNAPSHOT_DIR="${BASE}/myfs.img.snapshots"

echo "copying idkfsd & idkfsctl binaries"
cp "${BASE}/target/release/idkfsd" "${BIN_DIR}/idkfsd"
cp "${BASE}/target/release/idkfsctl" "${BIN_DIR}/idkfsctl"

echo "installing systemd unit"
cp "${SERVICE_SRC}" "${SERVICE_DST}"
chmod 644 "${SERVICE_DST}"

echo "installing OpenRC script"
install -d /etc/init.d
cp "${OPENRC_SRC}" "${OPENRC_DST}"
chmod +x "${OPENRC_DST}"

echo "writing configuration"
cat <<EOF > "${CONF}"
IDKFS_IMAGE=${BASE}/myfs.img
IDKFS_MOUNT=/home/cat/myfs
EOF

echo "creating snapshot store"
mkdir -p "${SNAPSHOT_DIR}"

echo "reloading systemd & starting idkfsd"
systemctl daemon-reload
systemctl enable --now idkfsd
systemctl status idkfsd --no-pager

echo "done! Run 'idkfsctl --socket /run/idkfsd.sock list' to see snapshots."
