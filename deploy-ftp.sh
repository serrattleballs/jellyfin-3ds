#!/bin/bash
# Deploy jellyfin-3ds to 3DS SD card via FTP
#
# Usage: ./deploy-ftp.sh <3DS_IP> [port]
#
# Requires: 3DS running an FTP server (FTPD, ftpd-classic, etc.)
# Copies jellyfin-3ds.3dsx to sdmc:/3ds/jellyfin-3ds/

set -e

IP="${1:?Usage: $0 <3DS_IP> [port]}"
PORT="${2:-5000}"

THREEDS_X="$(cd "$(dirname "$0")" && pwd)/jellyfin-3ds.3dsx"

if [ ! -f "$THREEDS_X" ]; then
    echo "No .3dsx found. Building first..."
    "$(dirname "$0")/build.sh"
fi

echo "Deploying to 3DS at $IP:$PORT via FTP..."
curl -s -T "$THREEDS_X" "ftp://$IP:$PORT/3ds/jellyfin-3ds/jellyfin-3ds.3dsx" --ftp-create-dirs && \
    echo "Done! Relaunch from Homebrew Launcher." || \
    echo "FTP failed. Check that FTPD is running on the 3DS."
