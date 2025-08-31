#!/bin/bash

# Step 1: Create a 2MB disk image with .dmg extension
hdiutil create -size 2m -fs "MS-DOS FAT12" -volname LOOPMOTION fat.img.dmg

# Step 2: Mount the disk image
# hdiutil attach creates a /dev/diskX entry and mounts it
MOUNT_POINT=$(hdiutil attach fat.img.dmg | grep -o "/Volumes/LOOPMOTION.*" | head -n 1)

if [ -z "$MOUNT_POINT" ]; then
  echo "Error: Failed to mount the disk image."
  exit 1
fi

# Step 3: Copy the config.htm file to the mounted volume
cp ../webconfig/config.htm "$MOUNT_POINT/config.htm"

# Step 4: Unmount the disk image
hdiutil detach "$MOUNT_POINT"

# Step 5: Convert the fat.img.dmg to disk.img (first 128 sectors, 512 bytes each = 64KB)
dd if=fat.img.dmg of=disk.img bs=512 count=128

# Step 6: Remove the temporary fat.img.dmg
rm fat.img.dmg