#!/bin/sh
# Create sdcard.img (see genimage.cfg) and the RAUC bundle uvrgw-rpi0w.raucb.

set -u
set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# own DT overlays
dtc -@ -I dts -O dtb -o "${BINARIES_DIR}/rpi-firmware/overlays/uart-rts.dtbo" \
	"${BOARD_DIR}/uart-rts-overlay.dts"

rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${BOARD_DIR}/data" \
	--tmppath "${GENIMAGE_TMP}" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${BOARD_DIR}/genimage.cfg"

# RAUC bundle with the root filesystem
KEYS_DIR="${BR2_EXTERNAL_UVRGW_PATH}/../keys"
VERSION="$(git -C "${BR2_EXTERNAL_UVRGW_PATH}" describe --always --dirty)"
BUNDLE_DIR="${BUILD_DIR}/rauc-bundle"

rm -rf "${BUNDLE_DIR}" "${BINARIES_DIR}/uvrgw-rpi0w.raucb"
mkdir -p "${BUNDLE_DIR}"
sed "s/@VERSION@/${VERSION}/" "${BOARD_DIR}/manifest.raucm.in" \
	> "${BUNDLE_DIR}/manifest.raucm"
cp "${BINARIES_DIR}/rootfs.squashfs" "${BUNDLE_DIR}/"

rauc bundle \
	--cert "${KEYS_DIR}/cert.pem" \
	--key "${KEYS_DIR}/key.pem" \
	"${BUNDLE_DIR}" "${BINARIES_DIR}/uvrgw-rpi0w.raucb"
