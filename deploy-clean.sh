#!/bin/bash
# Clean deploy script that skips absent floppy image and deploys built app to
# the 20MB image. Use this in WSL if the repo's vmac/Mac-800K.dsk is missing.
set -e

cd "$(dirname "$0")"

HD_IMAGE="vmac/ArtfulType_20M.dsk"
FLOPPY_IMAGE="vmac/Mac-800K.dsk"
FLOPPY_IMAGE_NAME="ArtfulType 800K"
BIN="app/build/ArtfulType.bin"
GUIDE="doc/START_HERE.md"
GUIDE_NAME="START_HERE.md"
WRAPPED_FLOPPY_TEMP="vmac/.ArtfulType_Floppy_wrapped.img"

echo "Building..."
cmake --build app/build

if [ -f "$FLOPPY_IMAGE" ]; then
    echo "Wrapping the 800K floppy image in a DiskCopy 4.2 header..."
    python3 make_diskcopy_image.py "$FLOPPY_IMAGE" "$WRAPPED_FLOPPY_TEMP" "The Artful Type"
fi

echo "Deploying to 20MB image..."
hmount "$HD_IMAGE"
hdel ArtfulType 2>/dev/null || true
hcopy -m "$BIN" :ArtfulType

if [ -f "$FLOPPY_IMAGE" ]; then
    hdel "$FLOPPY_IMAGE_NAME" 2>/dev/null || true
    hcopy -r "$WRAPPED_FLOPPY_TEMP" ":$FLOPPY_IMAGE_NAME"
    hattrib -t dImg -c dCpy ":$FLOPPY_IMAGE_NAME"
fi

hdel "$GUIDE_NAME" 2>/dev/null || true
hcopy -t "$GUIDE" ":$GUIDE_NAME"
hattrib -t TEXT -c ArtT ":$GUIDE_NAME"
humount

if [ -f "$WRAPPED_FLOPPY_TEMP" ]; then
    rm -f "$WRAPPED_FLOPPY_TEMP"
fi

if [ -f "$FLOPPY_IMAGE" ]; then
    echo "Deploying to 800K image..."
    hmount "$FLOPPY_IMAGE"
    hdel ArtfulType 2>/dev/null || true
    hcopy -m "$BIN" :ArtfulType
    hdel "$GUIDE_NAME" 2>/dev/null || true
    hcopy -t "$GUIDE" ":$GUIDE_NAME"
    hattrib -t TEXT -c ArtT ":$GUIDE_NAME"
    humount
else
    echo "Warning: $FLOPPY_IMAGE not found; skipped floppy deploy."
fi

echo "Done."
