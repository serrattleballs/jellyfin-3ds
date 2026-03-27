# Jellyfin 3DS

Native Jellyfin media client for Nintendo 3DS. Stream music and video from your Jellyfin server directly to your 3DS.

**The first native Jellyfin client for Nintendo 3DS.**

## Features

- **Music streaming** — browse artists, albums, tracks. MP3/AAC with album art
- **Video streaming** — H.264 hardware decode at 24fps on New 3DS (400x224)
- **Audio + video sync** — 3-thread architecture with audio-master sync
- **Library browsing** — navigate all Jellyfin libraries with pagination
- **Search** — find content across all libraries (SELECT button)
- **Seeking** — skip forward/back 30 seconds (L/R shoulder buttons)
- **Auto-play** — next track/episode plays automatically
- **Session persistence** — login once, credentials saved to SD card
- **Watch mode** — hide bottom screen for distraction-free viewing (D-pad Down)
- **Album art** — displayed on now-playing screen for music

## Requirements

- Nintendo 3DS with [Luma3DS](https://github.com/LumaTeam/Luma3DS) CFW
- [Homebrew Launcher](https://github.com/devkitPro/3ds-hbmenu)
- Jellyfin server on the same network
- **New 3DS** required for video playback (Old 3DS: audio only)

## Install

1. Download `jellyfin-3ds.3dsx` from [Releases](../../releases)
2. Copy to `sdmc:/3ds/jellyfin-3ds/jellyfin-3ds.3dsx` on your SD card
3. Launch from Homebrew Launcher

## Usage

### Controls

| Screen | Button | Action |
|--------|--------|--------|
| Browse | D-pad | Navigate list |
| Browse | A | Enter folder / Play |
| Browse | B | Go back |
| Browse | L/R | Next/previous page |
| Browse | SELECT | Search |
| Browse | Touch | Tap to select, drag to scroll |
| Now Playing | A | Pause / Resume |
| Now Playing | X | Stop |
| Now Playing | B | Back to browse |
| Now Playing | L/R | Seek ±30 seconds |
| Now Playing | D-pad Down | Watch mode (hide controls) |
| Now Playing | D-pad Up | Show controls |
| Any | START | Exit app |

### First Launch

1. Enter your Jellyfin server URL (e.g. `http://192.168.1.100:8096`)
2. Enter username and password
3. Press R to connect

Credentials are saved automatically — next launch skips login.

## Building from Source

### Prerequisites

- [devkitPro](https://devkitpro.org/) with 3DS toolchain
- Docker (for reproducible builds)

### Build

```bash
# Install devkitPro packages (if building natively)
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib \
    3ds-libmpg123 3ds-libopus 3ds-opusfile 3ds-libvorbisidec 3ds-libogg

# Build FFmpeg (first time only)
./lib/ffmpeg/build-ffmpeg.sh docker

# Build the app
./build.sh

# Deploy via FTP (3DS must be running ftpd)
./deploy-ftp.sh <3DS_IP> 5000
```

### Docker build (recommended)

```bash
./build.sh  # uses devkitpro/devkitarm Docker image
```

## Architecture

```
Network thread → Ring buffer → Decode thread (FFmpeg demux + MVD H.264 + AAC)
    → Frame queue (6 slots) → Convert thread (Morton tile + A/V sync)
    → Double-buffered GPU textures → Main thread (citro2d render)
```

- **FFmpeg 6.x** — MPEG-TS demuxer, AAC decoder, H.264 parser
- **MVD** — Hardware H.264 decoder (New 3DS)
- **NDSP** — Hardware audio output
- **citro2d/citro3d** — GPU-accelerated 2D rendering
- **libcurl + mbedTLS** — HTTPS networking
- **cJSON** — JSON API parsing
- **stb_image** — Album art JPEG decoding

## Known Limitations

- Video playback requires **New 3DS** (Old 3DS: audio only)
- No subtitle support yet
- No offline/download mode
- CIA install not yet working (use .3dsx via Homebrew Launcher)
- Some library types may show unexpected content (server-dependent)

## Credits

Built with reference to:
- [ThirdTube](https://github.com/windows-server-2003/ThirdTube) — video architecture
- [Switchfin](https://github.com/dragonflylee/switchfin) — Jellyfin API patterns
- [Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS) — MVD decoder usage

## License

MIT
