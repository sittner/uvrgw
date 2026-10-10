#!/bin/sh
# Link the writable configuration and state into /data (rootfs is read-only),
# install the RAUC keyring.

set -u
set -e

KEYS_DIR="${BR2_EXTERNAL_UVRGW_PATH}/../keys"

if [ ! -f "${KEYS_DIR}/cert.pem" ]; then
	echo "RAUC certificate ${KEYS_DIR}/cert.pem missing (see README)" >&2
	exit 1
fi
install -D -m 0644 "${KEYS_DIR}/cert.pem" "${TARGET_DIR}/etc/rauc/keyring.pem"

mkdir -p "${TARGET_DIR}/data" "${TARGET_DIR}/etc/wpa_supplicant" \
	"${TARGET_DIR}/etc/systemd/network"

ln -sfn /data/wpa_supplicant-wlan0.conf \
	"${TARGET_DIR}/etc/wpa_supplicant/wpa_supplicant-wlan0.conf"
ln -sfn /data/wlan0.network "${TARGET_DIR}/etc/systemd/network/wlan0.network"
ln -sfn /data/dropbear "${TARGET_DIR}/etc/dropbear"
ln -sfn /data/ssh "${TARGET_DIR}/root/.ssh"
