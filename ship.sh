#!/bin/bash
# Build and deploy jellyfin-3ds CIA to 3DS via FTP
#
# Usage: ./ship.sh <3DS_IP> [ftp_port]
#
# Does: build .3dsx → build .cia → FTP upload to /cias/ on SD card
# Then: open FBI on 3DS → SD → cias → jellyfin-3ds.cia → Install

set -e
cd "$(dirname "$0")"

IP="${1:?Usage: $0 <3DS_IP> [ftp_port]}"
PORT="${2:-5000}"

echo "=== Building ==="
./build.sh

echo ""
echo "=== Building CIA ==="
./build-cia.sh

echo ""
echo "=== Uploading via FTP ==="
curl -T jellyfin-3ds.cia "ftp://$IP:$PORT/jellyfin-3ds.cia" && \
    echo "" && echo "Uploaded to sdmc:/jellyfin-3ds.cia" && \
    echo "Open FBI → SD → jellyfin-3ds.cia → Install and delete CIA" || \
    echo "FTP failed — is ftpd running on the 3DS?"
