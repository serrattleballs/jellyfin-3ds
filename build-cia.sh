#!/bin/bash
# Build jellyfin-3ds.cia for home screen installation
#
# Usage: ./build-cia.sh
#
# Requires: makerom, bannertool (installed on Proxmox)
# Requires: jellyfin-3ds.elf (run ./build.sh first)

set -e
cd "$(dirname "$0")"

ELF="jellyfin-3ds.elf"
SMDH="jellyfin-3ds.smdh"
CIA="jellyfin-3ds.cia"
RSF="app.rsf"

if [ ! -f "$ELF" ]; then
    echo "No .elf found. Run ./build.sh first."
    exit 1
fi

if [ ! -f "$SMDH" ]; then
    echo "No .smdh found. Run ./build.sh first."
    exit 1
fi

# Get the icon — use the SMDH icon or default
ICON="$SMDH"

# Create banner
echo "Creating banner..."
bannertool makebanner \
    -i assets/icons/banner.png 2>/dev/null \
    -a assets/audio_silent.wav \
    -o banner.bnr 2>/dev/null || \
bannertool makebanner \
    -ci assets/audio_silent.wav \
    -o banner.bnr 2>/dev/null || {
    # Fallback: create a minimal banner without an image
    echo "bannertool failed — creating banner with cgfx fallback"
    bannertool makebanner -ca -o banner.bnr 2>/dev/null || true
}

# Create CIA
echo "Building CIA..."
makerom -f cia \
    -o "$CIA" \
    -elf "$ELF" \
    -rsf "$RSF" \
    -icon "$SMDH" \
    -banner banner.bnr \
    -exefslogo \
    -target t

if [ -f "$CIA" ]; then
    echo ""
    echo "=== CIA Built ==="
    ls -lh "$CIA"
    echo ""
    echo "Install via FBI on 3DS:"
    echo "  1. Copy $CIA to SD card"
    echo "  2. Open FBI → SD → navigate to $CIA → Install"
    echo "  Or install remotely via FBI network install"
else
    echo "CIA build failed."
    exit 1
fi
