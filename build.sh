#!/bin/bash
# Build jellyfin-3ds using devkitPro Docker image
# Usage: ./build.sh [clean]

set -e

IMAGE="devkitpro/devkitarm:latest"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
MOUNT="/src/jellyfin-3ds"

if [ "$1" = "clean" ]; then
    docker run --rm -v "$PROJECT_DIR:$MOUNT" -w "$MOUNT" "$IMAGE" make clean
fi

docker run --rm -v "$PROJECT_DIR:$MOUNT" -w "$MOUNT" "$IMAGE" make

echo ""
echo "Output: $PROJECT_DIR/jellyfin-3ds.3dsx"
ls -lh "$PROJECT_DIR/jellyfin-3ds.3dsx"
