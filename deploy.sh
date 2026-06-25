#!/bin/bash
# Builds ArtfulType and deploys it to both test images. The 800K floppy
# image is also copied onto the 20MB image (raw, not MacBinary -- it's
# a disk image, not a Mac file with forks) so that booting the 20MB
# image on real BlueSCSI hardware lets you write an actual physical
# floppy from the file sitting on disk.
set -e

cd "$(dirname "$0")"

HD_IMAGE="vmac/ArtfulType_20M.dsk"
FLOPPY_IMAGE="vmac/Mac-800K.dsk"
FLOPPY_IMAGE_NAME="ArtfulType Floppy.dsk"
BIN="app/build/ArtfulType.bin"

echo "Building..."
cmake --build app/build

echo "Deploying to 20MB image..."
hmount "$HD_IMAGE"
hdel ArtfulType 2>/dev/null || true
hcopy -m "$BIN" :ArtfulType
hdel "$FLOPPY_IMAGE_NAME" 2>/dev/null || true
hcopy -r "$FLOPPY_IMAGE" ":$FLOPPY_IMAGE_NAME"
humount

echo "Deploying to 800K image..."
hmount "$FLOPPY_IMAGE"
hdel ArtfulType 2>/dev/null || true
hcopy -m "$BIN" :ArtfulType
humount

echo "Done."
