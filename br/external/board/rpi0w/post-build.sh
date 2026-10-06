#!/bin/sh
# Link the writable configuration and state into /data (rootfs is read-only).

set -u
set -e

mkdir -p "${TARGET_DIR}/data" "${TARGET_DIR}/etc/wpa_supplicant"

ln -sfn /data/wpa_supplicant-wlan0.conf \
	"${TARGET_DIR}/etc/wpa_supplicant/wpa_supplicant-wlan0.conf"
ln -sfn /data/dropbear "${TARGET_DIR}/etc/dropbear"
ln -sfn /data/ssh "${TARGET_DIR}/root/.ssh"
