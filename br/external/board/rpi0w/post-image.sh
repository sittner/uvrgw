#!/bin/sh
# Create sdcard.img (see genimage.cfg).

set -u
set -e

BOARD_DIR="$(dirname "$0")"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

cp "${BOARD_DIR}/autoboot.txt" "${BOARD_DIR}/cmdline-a.txt" \
	"${BOARD_DIR}/cmdline-b.txt" "${BINARIES_DIR}/"

# empty rootpath: the data partition starts empty, the other images are
# prebuilt
ROOTPATH_TMP="$(mktemp -d)"
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT

rm -rf "${GENIMAGE_TMP}"

genimage \
	--rootpath "${ROOTPATH_TMP}" \
	--tmppath "${GENIMAGE_TMP}" \
	--inputpath "${BINARIES_DIR}" \
	--outputpath "${BINARIES_DIR}" \
	--config "${BOARD_DIR}/genimage.cfg"
