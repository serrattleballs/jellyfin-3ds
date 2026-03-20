# Architecture

## System Diagram

```
┌─────────────────────────────────────────────────────┐
│                    main.c                           │
│  init → [ input → update → render → vblank ] → exit│
├──────────┬──────────────┬───────────┬───────────────┤
│  UI      │ Audio Player │ API Client│ Config        │
│  citro2d │              │ libcurl   │ SD card INI   │
│  touch   │  ┌─────────┐│ cJSON     │               │
│  dpad    │  │Net thrd  ││           │               │
│          │  │curl ──────>│           │               │
│          │  │  ↓ ring   ││           │               │
│          │  │Dec thrd  ││           │               │
│          │  │mpg123    ││           │               │
│          │  │  ↓ PCM16 ││           │               │
│          │  │NDSP DMA  ││           │               │
│          │  └─────────┘││           │               │
├──────────┴──────────────┴───────────┴───────────────┤
│           libctru (3DS OS services)                 │
│  gfx · soc · ndsp · httpc · fs · hid · apt · swkbd │
├─────────────────────────────────────────────────────┤
│           3DS Hardware                              │
│  ARM11 · PICA200 GPU · DSP · WiFi · Touchscreen    │
└─────────────────────────────────────────────────────┘
```

## Threading Model

The 3DS has limited threading (Old 3DS: 2 cores, New 3DS: 4 cores). We use 3 threads:

| Thread | CPU | Purpose |
|--------|-----|---------|
| Main | Core 0 | Input polling, UI update, GPU rendering |
| Network | Core 1 | libcurl HTTP streaming into ring buffer |
| Decode | Core 1 | mpg123 decode from ring buffer, feed NDSP |

NDSP runs on the DSP coprocessor (not an ARM core) — it consumes PCM buffers via DMA independently.

## Memory Budget (Old 3DS: 64MB)

| Component | Budget | Notes |
|-----------|--------|-------|
| Application code + stack | ~2 MB | |
| libcurl + TLS | ~1 MB | mbedTLS session state |
| Network ring buffer | 512 KB | Audio prefetch |
| NDSP PCM buffers | 128 KB | 4 × 4096 samples × 2ch × 16bit |
| citro2d render state | ~2 MB | GPU command buffers |
| Album art textures | ~512 KB | 3 × 128x128 RGBA8 (phase 2) |
| Item list data | ~100 KB | 50 items × ~2KB each |
| Socket buffer | 1 MB | SOC service allocation |
| **Total** | **~7 MB** | Leaves 57MB headroom |

## Data Flow: Playing a Track

```
1. User selects audio item in UI
2. ui_update() calls jfin_get_audio_stream() → builds stream URL
3. ui_update() calls audio_player_play(url)
4. audio_player_play() spawns net_thread + decode_thread
5. net_thread: curl fetches URL → writes to ring buffer
6. decode_thread: waits for AUDIO_PREFETCH_BYTES in ring buffer
7. decode_thread: reads ring → mpg123_feed() → mpg123_read() → PCM16
8. decode_thread: fills ndspWaveBuf → ndspChnWaveBufAdd() → DSP plays
9. Main thread: audio_player_get_status() → ui_render_now_playing()
10. Main thread: periodically calls jfin_report_progress()
```

## API Authentication

Every request carries a `MediaBrowser` authorization header:

```
Authorization: MediaBrowser Client="Jellyfin 3DS", Device="Nintendo 3DS",
  DeviceId="<unique>", Version="0.1.0"[, Token="<access-token>"]
```

Login via `POST /Users/AuthenticateByName` returns an `AccessToken` that persists in SD card config.

## Design Decisions

**Why C instead of C++?**
- Smaller binary (no RTTI, no exceptions, no STL)
- Matches libctru's C API style
- cJSON is C; mpg123 is C; curl is C
- ThirdTube uses C++ but its binary is large and build is complex

**Why libcurl instead of libctru httpc?**
- Connection pooling (TLS handshakes are expensive on ARM11)
- Redirect following (Jellyfin may redirect for transcoded streams)
- Chunked transfer encoding support
- Battle-tested streaming performance (ThirdTube validates this)

**Why MP3 as default transcode target?**
- mpg123 is extremely lightweight to decode
- Every Jellyfin server can transcode to MP3
- 128kbps MP3 is ~16KB/s — well within WiFi budget
- Opus would be better quality but decode is heavier

**Why single curl handle for API, separate for streaming?**
- API calls are short-lived, sequential from main thread
- Audio streaming is long-lived, runs in a background thread
- Separate handles avoid contention and simplify lifecycle
