#!/bin/bash
# Deploy jellyfin-3ds to a 3DS over WiFi via 3dslink
#
# Usage:
#   ./deploy.sh <3DS_IP>
#   ./deploy.sh <3DS_IP>
#
# Prerequisites:
#   - 3DS with Luma3DS CFW
#   - Open Homebrew Launcher on the 3DS (it listens for 3dslink)
#   - 3DS and this machine on the same network
#
# The -s flag starts a debug server so printf() output from the 3DS
# is streamed back to this terminal.

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <3DS_IP_ADDRESS>"
    echo ""
    echo "Open Homebrew Launcher on your 3DS first."
    echo "The IP address is shown on the bottom screen."
    exit 1
fi

THREEDSX="$(cd "$(dirname "$0")" && pwd)/jellyfin-3ds.3dsx"

if [ ! -f "$THREEDSX" ]; then
    echo "No .3dsx found. Building first..."
    "$(dirname "$0")/build.sh"
fi

echo "Deploying to 3DS at $1..."
docker run --rm --network host \
    -v "$(dirname "$THREEDSX"):/src/jellyfin-3ds" \
    devkitpro/devkitarm \
    3dslink /src/jellyfin-3ds/jellyfin-3ds.3dsx -a "$1" -s
