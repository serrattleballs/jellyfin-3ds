# Video Streaming — Research & Implementation Plan

> Research date: 2026-03-22
> Status: **Planning** — not yet in development
> Target: New 3DS only (Old 3DS lacks hardware H.264 decode)

## Table of Contents

- [1. Overview](#1-overview)
- [2. Reference Implementations](#2-reference-implementations)
- [3. Architecture](#3-architecture)
- [4. Jellyfin Transcoding Strategy](#4-jellyfin-transcoding-strategy)
- [5. MVD Hardware Decoder](#5-mvd-hardware-decoder)
- [6. FFmpeg Cross-Compilation](#6-ffmpeg-cross-compilation)
- [7. Threading Model](#7-threading-model)
- [8. Memory Budget](#8-memory-budget)
- [9. A/V Synchronization](#9-av-synchronization)
- [10. Frame Rendering Pipeline](#10-frame-rendering-pipeline)
- [11. Implementation Phases](#11-implementation-phases)
- [12. Risk Register](#12-risk-register)
- [13. Open Questions](#13-open-questions)
- [14. References](#14-references)

---

## 1. Overview

### Goal

Stream video from a Jellyfin server to the Nintendo 3DS top screen (400x240) with synchronized audio on the bottom screen controls, using the New 3DS MVD hardware H.264 decoder.

### Constraints

| Constraint | Value |
|---|---|
| Top screen resolution | 400x240 |
| Hardware decoder | MVD (New 3DS only) — H.264 Baseline/Main/High through Level 3.2 |
| WiFi bandwidth | ~15-20 Mbps (New 3DS 802.11n) |
| Linear memory available | ~30 MB (shared with OS, audio, UI) |
| Target video bitrate | ~500 kbps (comfortable margin for WiFi) |
| Target audio | AAC-LC stereo 48kHz 128kbps |
| Container | MPEG Transport Stream (TS) |

### What Already Works

- Jellyfin API client (auth, browse, stream URLs, playback reporting)
- Audio streaming pipeline (curl → ring buffer → mpg123 → NDSP)
- Dual-screen citro2d UI
- Build system (devkitPro Docker)

---

## 2. Reference Implementations

### ThirdTube (YouTube client for 3DS)

**Repo:** https://github.com/windows-server-2003/ThirdTube

The closest prior art. Streams YouTube video over HTTPS, demuxes with FFmpeg, decodes H.264 via MVD, renders with citro2d. Key takeaways:

- Uses a **custom FFmpeg cross-compile** with minimal codecs enabled (`aac`, `h264`, `opus`, `mov` demuxer)
- Two patches to FFmpeg: force-enable pthreads, fix devkitARM enum sizing
- **fake_pthread shim** (~200 lines) maps POSIX pthreads → libctru primitives
- Achieves **360p on New 3DS** with hardware decode, 144p on Old 3DS (software fallback)
- **Audio-master A/V sync**: video thread sleeps until audio PTS catches up
- 4 threads: network download, packet decode, frame convert, livestream init
- Pre-built FFmpeg static libs total ~4MB

### Video player for 3DS

**Repo:** https://github.com/Core-2-Extreme/Video_player_for_3DS

Local file player (no streaming). Most detailed MVD usage, including:

- **Sentinel-based frame detection**: writes marker bytes at frame corners before MVD call to detect when a frame was produced during NAL processing
- **AVCC → Annex B conversion**: replaces 4-byte big-endian length prefixes with `00 00 01` start codes
- **SPS/PPS extraction** from FFmpeg `extradata` field
- **First-frame quirk**: feeds the first NAL unit twice to prime the decoder pipeline
- Work buffer size calculation based on H.264 level and reference frames

### Switchfin (Jellyfin client for Switch/Vita)

**Repo:** https://github.com/dragonflylee/switchfin

Reference for Jellyfin-specific integration:

- DeviceProfile structure for `POST /Items/{id}/PlaybackInfo`
- PS Vita profile constrains to H.264 Baseline, Level ≤ 4.0
- Uses HLS with TS container for transcoding
- Quality presets: 420kbps→360p, 720kbps→480p

---

## 3. Architecture

### Data Flow

```
Jellyfin Server                              New 3DS
┌──────────────┐   WiFi    ┌────────────────────────────────────┐
│ FFmpeg encode │          │                                    │
│ H.264 BL     │ TS stream│  Network Thread (curl)             │
│ 400x240      ├─────────→│    ↓                               │
│ AAC stereo   │          │  Ring Buffer (512KB)               │
│ ~600 kbps    │          │    ↓                               │
└──────────────┘          │  FFmpeg avformat (TS demux)        │
                          │    ↓ audio          ↓ video        │
                          │  FFmpeg AAC      AVCC→Annex B      │
                          │  decode          conversion        │
                          │    ↓                ↓              │
                          │  swr_convert     MVD HW decode     │
                          │  → PCM16         → BGR565          │
                          │    ↓                ↓              │
                          │  NDSP            GPU texture       │
                          │  (audio out)     (citro2d sprite)  │
                          │                      ↓             │
                          │               Top Screen 400x240   │
                          │  Bottom Screen: transport controls │
                          └────────────────────────────────────┘
```

### Module Boundaries

```
src/video/
  ffmpeg_demux.c      TS demux via avformat, packet routing
  mvd_decode.c        MVD init, NAL feeding, frame extraction
  video_render.c      GPU texture upload, top screen display
  video_player.c      Orchestrator: threads, A/V sync, state machine
  fake_pthread.c      POSIX → libctru thread primitive mapping

include/video/
  ffmpeg_demux.h      Demuxer API
  mvd_decode.h        MVD decoder API
  video_render.h      Frame display API
  video_player.h      Public player API (play/stop/pause/seek)
```

---

## 4. Jellyfin Transcoding Strategy

### Verified Configuration

Tested against Jellyfin 10.11.5 at `http://your-jellyfin-server:8096` with "2001: A Space Odyssey" (HEVC Main 10, HDR10, 1920x872, MKV).

**Server output:** H.264 Constrained Baseline, Level 3.1, 400xN (aspect-corrected), 0 B-frames, 1 ref frame, yuv420p 8-bit, AAC-LC stereo 48kHz 128kbps.

### MVP: Progressive TS Stream

```
GET /Videos/{id}/stream.ts?api_key={key}
    &VideoCodec=h264
    &AudioCodec=aac
    &VideoBitrate=472000
    &AudioBitrate=128000
    &MaxWidth=400
    &MaxHeight=240
    &Profile=Baseline
    &Level=31
    &MaxRefFrames=4
    &TranscodingMaxAudioChannels=2
    &MediaSourceId={id}
```

**Why progressive TS over HLS for MVP:**
- No m3u8 parser needed
- Single HTTP request, single curl handle
- Reuses existing ring buffer pattern from audio player
- TS packets are 188 bytes — easy to parse incrementally

**Why not MP4:**
- MP4 requires moov atom parsing before playback can start
- TS is streamable from byte 0

### Future: HLS for Seeking

```
GET /Videos/{id}/master.m3u8?api_key={key}
    &VideoCodec=h264&AudioCodec=aac
    &VideoBitrate=472000&AudioBitrate=128000
    &MaxWidth=400&MaxHeight=240
    &Profile=Baseline&Level=31
    &SegmentContainer=ts&SegmentLength=6
    &TranscodingMaxAudioChannels=2
    &MediaSourceId={id}
```

HLS advantages: segment-based seeking, error recovery per segment, better for unreliable WiFi. Deferred to post-MVP.

### Critical Parameters

| Parameter | Value | Why |
|---|---|---|
| `Profile=Baseline` | Forces Constrained Baseline | No B-frames, no CABAC — simplest for MVD |
| `Level=31` | H.264 Level 3.1 | Supports 400x240 with headroom; without this, server defaults to Level 4.1 |
| `MaxRefFrames=4` | ≤4 reference frames | 400x240 MVD supports up to 8, but 4 uses less work buffer memory |
| `MaxWidth=400` | Top screen width | Server aspect-ratio-corrects height automatically |
| `VideoBitrate=472000` | ~472 kbps video | Total 600kbps with audio; well within WiFi budget |

### Subtitle Options

| Method | How | MVP? |
|---|---|---|
| Burn-in (`SubtitleMethod=Encode`) | Server renders into video stream | Yes — simplest, no client-side rendering |
| External SRT (`SubtitleMethod=External`) | Fetch `/Videos/{id}/{src}/Subtitles/{idx}/0/Stream.srt` | Later — requires text rendering |

---

## 5. MVD Hardware Decoder

### API Summary

```c
// Init — allocate ~10MB work buffer
mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_BGR565, workbuf_size, NULL);

// Generate default config for our resolution
MVDSTD_Config config;
mvdstdGenerateDefaultConfig(&config, 400, 240, 400, 240, NULL, outbuf, outbuf);

// Feed SPS (must be first)
// Prefix with 00 00 01, copy SPS NAL data from extradata
GSPGPU_FlushDataCache(inbuf, sps_size);
mvdstdProcessVideoFrame(inbuf, sps_size, 0, NULL);

// Feed PPS (must be second)
GSPGPU_FlushDataCache(inbuf, pps_size);
mvdstdProcessVideoFrame(inbuf, pps_size, 0, NULL);

// Decode loop — for each NAL unit:
//   1. Convert AVCC → Annex B in linear memory
//   2. Flush cache
//   3. Process NAL unit
//   4. Render frame if ready
GSPGPU_FlushDataCache(inbuf, nal_size);
config.physaddr_outdata0 = osConvertVirtToPhys(outbuf);
MVDSTD_SetConfig(&config);
Result ret = mvdstdProcessVideoFrame(inbuf, nal_size, 0, NULL);
if (ret != MVD_STATUS_PARAMSET) {
    mvdstdRenderVideoFrame(NULL, true);  // blocks until frame ready
    // outbuf now contains BGR565 pixel data
}

// Cleanup
mvdstdExit();
```

### AVCC → Annex B Conversion

FFmpeg outputs H.264 in AVCC format (4-byte big-endian length per NAL unit). MVD requires Annex B (start code prefix). Conversion:

```c
while (source_offset + 4 < packet_size) {
    int nal_size = __builtin_bswap32(*(uint32_t*)(avcc_data + source_offset));
    source_offset += 4;

    // Write Annex B start code
    annexb_buf[offset++] = 0x00;
    annexb_buf[offset++] = 0x00;
    annexb_buf[offset++] = 0x01;

    // Copy NAL unit data
    memcpy(annexb_buf + offset, avcc_data + source_offset, nal_size);
    offset += nal_size;
    source_offset += nal_size;
}
```

### SPS/PPS Extraction from FFmpeg extradata

```c
// AVCC extradata layout:
// [0]    version (1)
// [1-3]  profile, compat, level
// [4]    length_size_minus_one (lower 2 bits)
// [5]    num_sps (lower 5 bits)
// [6-7]  sps_length (big-endian)
// [8..]  sps_data
// [..]   num_pps
// [..]   pps_length (big-endian)
// [..]   pps_data

uint8_t *ed = codec_ctx->extradata;
int sps_len = (ed[6] << 8) | ed[7];
uint8_t *sps_data = ed + 8;
int pps_offset = 8 + sps_len + 1;  // +1 for num_pps byte
int pps_len = (ed[pps_offset] << 8) | ed[pps_offset + 1];
uint8_t *pps_data = ed + pps_offset + 2;
```

### Known Quirks

1. **First-frame double-feed**: Send the first video NAL unit twice. Discard the first output frame (may contain stale data from a previous session).
2. **1-frame pipeline delay**: Feed frame N, receive frame N-1. Use sentinel bytes at frame corners to detect when MVD produces output during `ProcessVideoFrame`.
3. **Width/height alignment**: Both must be aligned to 16-pixel boundaries.
4. **Linear memory only**: Input buffer, output buffer, and work buffer must all be in linear memory (`linearAlloc`).
5. **Cache flush required**: `GSPGPU_FlushDataCache(buf, size)` before every `ProcessVideoFrame` call.

---

## 6. FFmpeg Cross-Compilation

### Configure Command

Based on ThirdTube's proven configuration, extended for our needs:

```bash
./configure \
    --enable-cross-compile \
    --cross-prefix=arm-none-eabi- \
    --prefix=$BUILD_DIR \
    --cpu=armv6k --arch=arm --target-os=linux \
    --extra-cflags="-mfloat-abi=hard -mtune=mpcore -mtp=cp15 -D_POSIX_THREADS \
                    -I$DEVKITPRO/libctru/include" \
    --extra-ldflags="-mfloat-abi=hard" \
    --disable-filters --disable-devices --disable-bsfs --disable-parsers \
    --disable-hwaccels --disable-debug --disable-programs \
    --disable-avdevice --disable-postproc \
    --disable-decoders --disable-demuxers --disable-encoders --disable-muxers \
    --disable-asm --disable-protocols \
    --enable-pthreads --enable-inline-asm --enable-vfp \
    --enable-armv5te --enable-armv6 \
    --enable-decoder="aac,h264" \
    --enable-demuxer="mpegts,mov" \
    --enable-protocol="file" \
    --enable-filter="aformat,aresample,anull"
```

**Differences from ThirdTube:**
- Added `mpegts` demuxer (ThirdTube only has `mov` for YouTube DASH)
- Removed `opus` decoder (not needed for Jellyfin)
- Removed audio effect filters (chorus, superequalizer, etc.)
- Kept `mov` demuxer for future MP4 support

### Required Patches

**Patch 1: Force-enable pthreads** (`configure`)
Replace pthread detection block with unconditional `enable pthreads`. The 3DS toolchain provides custom pthread via `fake_pthread.c`, not system pthreads.

**Patch 2: devkitARM enum fix** (`libavutil/samplefmt.h`, `libavutil/avassert.h`)
- Force enums to int-sized by adding `AV_SAMPLE_FORCE_INT = 0x7FFFFFFF` sentinel
- devkitARM uses small enums by default, causing ABI mismatches

### fake_pthread Shim

~200 lines mapping POSIX → libctru:

| POSIX | libctru |
|---|---|
| `pthread_mutex_*` | `LightLock_*` |
| `pthread_cond_*` | `CondVar_*` |
| `pthread_create` | `threadCreate` (round-robin core assignment) |
| `sysconf(_SC_NPROCESSORS_ONLN)` | 4 (New 3DS) |

### Expected Library Sizes

| Library | Size | Purpose |
|---|---|---|
| `libavformat.a` | ~500 KB | Container demuxing (TS, MP4) |
| `libavcodec.a` | ~2 MB | AAC decode, H.264 SW decode fallback |
| `libavutil.a` | ~850 KB | Common utilities |
| `libswresample.a` | ~100 KB | Audio sample format conversion |
| `libswscale.a` | ~450 KB | Not strictly needed (MVD outputs RGB) |
| **Total** | **~4 MB** | Added to binary size |

---

## 7. Threading Model

### Thread Layout (New 3DS — 4 cores)

| Thread | Core | Priority | Purpose |
|---|---|---|---|
| **Main** | 0 | Normal | Input polling, UI rendering, frame display |
| **Network** | 1 | High | curl HTTP streaming into ring buffer |
| **Demux + Decode** | 2 | High | FFmpeg demux, MVD NAL feeding, AAC decode |
| **Convert** | 0/1 | Normal | BGR565 → GPU texture, A/V sync wait |

The main thread owns the GPU. The convert thread prepares textures but the main thread issues the draw calls.

### Synchronization

- **Ring buffer** (network → demux): existing pattern, `LightLock` protected
- **Packet queues** (demux → decode): `std::deque<AVPacket*>` per stream, mutex protected
- **Frame ready signal** (decode → display): atomic flag or `LightEvent`
- **A/V sync** (decode → convert): audio PTS comparison with sleep loop

---

## 8. Memory Budget

### Linear Memory (New 3DS: ~30 MB available)

| Component | Size | Notes |
|---|---|---|
| MVD work buffer | ~10 MB | Level 3.1, 400x240, 4 ref frames |
| MVD input buffer | 256 KB | Largest expected NAL unit |
| MVD output frames (x2) | 384 KB | 400×240×2 bytes × 2 (double buffer) |
| Network ring buffer | 512 KB | Shared with existing audio path |
| NDSP PCM buffers | 128 KB | Existing audio buffers |
| FFmpeg internal state | ~2 MB | Codec contexts, packet buffers |
| **Total linear** | **~13 MB** | Leaves ~17 MB headroom |

### Heap Memory

| Component | Size |
|---|---|
| FFmpeg static libs (code) | ~4 MB |
| Packet queues | ~500 KB |
| Application + UI | ~2 MB |
| **Total heap** | **~6.5 MB** |

**Combined: ~20 MB** — well within New 3DS limits.

---

## 9. A/V Synchronization

### Strategy: Audio-Master

Audio playback drives the clock. Video frames are displayed when their PTS matches or falls behind the audio position. This is the same strategy used by ThirdTube.

```
audio_pts = ndsp_get_current_sample_position() / sample_rate + buffer_base_pts

for each decoded video frame:
    if video_pts - audio_pts > 3ms:
        sleep(video_pts - audio_pts - 1.5ms)  // wait for audio to catch up
    else if audio_pts - video_pts > 50ms:
        drop frame  // video is too far behind, skip to catch up
    else:
        display frame
```

### Audio Clock

`Util_speaker_get_current_timestamp()` from ThirdTube returns:
```
base_pts_of_current_buffer + ndspChnGetSamplePos(channel) / sample_rate
```

This gives sub-millisecond audio position accuracy.

### Edge Cases

| Situation | Handling |
|---|---|
| Audio buffer underrun | Display video immediately (no sync reference) |
| Video decode slower than realtime | Drop frames to catch up with audio |
| Network stall | Both audio and video pause; resume synced |
| Seek | Flush both pipelines, send new SPS/PPS, restart from keyframe |

---

## 10. Frame Rendering Pipeline

### MVD Output → Top Screen

1. MVD decodes to BGR565 in linear memory (`outbuf`)
2. Copy pixels into a citro3d `C3D_Tex` with Morton tiling (8×8 block swizzle)
3. Wrap texture as `C2D_Image`
4. Draw sprite on top screen: `C2D_DrawImageAt(vid_image, x, y, 0.5f, NULL)`

### Double Buffering

Two textures alternate: decode thread writes to `tex[write_idx]`, main thread renders from `tex[!write_idx]`. Swap index after each frame.

### Aspect Ratio

Jellyfin auto-corrects aspect ratio (e.g., 2.35:1 source → 400×170 output). The video sprite is centered vertically on the 400×240 top screen with letterboxing.

---

## 11. Implementation Phases

### Phase 1: FFmpeg Build (1-2 sessions)

- [ ] Fork ThirdTube's FFmpeg or use upstream with patches
- [ ] Write build script for devkitPro Docker
- [ ] Add `mpegts` demuxer, `aac` + `h264` decoders
- [ ] Write `fake_pthread.c` shim
- [ ] Verify static libs link into our project
- [ ] Milestone: `avformat_open_input` + `av_read_frame` compile and link

### Phase 2: Video Decode — No Audio (1-2 sessions)

- [ ] Implement `mvd_decode.c`: init, SPS/PPS, NAL feeding, frame extraction
- [ ] Implement `video_render.c`: BGR565 → GPU texture → top screen
- [ ] Implement `ffmpeg_demux.c`: open TS stream, extract video packets
- [ ] Write AVCC → Annex B converter
- [ ] Hardcode a test video URL, decode and display frames
- [ ] Milestone: see video frames on the 3DS top screen (no audio, no sync)

### Phase 3: A/V Sync (1 session)

- [ ] Route AAC audio packets through FFmpeg decode → swr_convert → NDSP
- [ ] Implement audio PTS clock
- [ ] Implement video-waits-for-audio sync loop
- [ ] Frame dropping when video falls behind
- [ ] Milestone: synchronized audio and video playback

### Phase 4: Integration (1 session)

- [ ] Add `jfin_get_video_stream_ts()` to API client
- [ ] Wire into UI: A on movie/episode starts video player
- [ ] Video on top screen, transport controls on bottom screen
- [ ] Pause/resume/stop
- [ ] Playback reporting to Jellyfin
- [ ] New 3DS detection: fall back to audio-only on Old 3DS
- [ ] Milestone: end-to-end video playback from Jellyfin browse → play

### Phase 5: Polish (future)

- [ ] Seeking (re-request stream with `StartTimeTicks`)
- [ ] Subtitle support (burn-in via server-side)
- [ ] HLS for better error recovery
- [ ] Buffer health indicator in UI
- [ ] Quality selection (bitrate presets)

---

## 12. Risk Register

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | FFmpeg cross-compile fails or produces broken libs | Medium | Blocks all video work | Use ThirdTube's pre-built libs as fallback; their FFmpeg fork is proven |
| 2 | MVD doesn't accept our NAL format | Low | Blocks decode | Follow Video_player_for_3DS's exact AVCC→Annex B code; test with known-good H.264 stream |
| 3 | A/V sync drift over long playback | Medium | Annoying but not fatal | ThirdTube's approach (audio-master + sleep) is proven; tune thresholds empirically |
| 4 | Not enough linear memory | Low | Crash | Budget shows ~17MB headroom; reduce MVD work buffer if needed |
| 5 | TS demuxer doesn't handle Jellyfin's output | Low | No video packets | Verified Jellyfin output with ffprobe; standard MPEG-TS compliance |
| 6 | WiFi throughput insufficient at 600kbps | Low | Buffering | Tested audio at 128kbps without issues; 600kbps is well within ~15Mbps WiFi |
| 7 | Frame rendering too slow (Morton tiling copy) | Low-Medium | Choppy video | ThirdTube achieves 360p; 400x240 is less pixels. DMA copy as fallback |
| 8 | Old 3DS users expect video | N/A | Feature gap | Clear messaging: "Video requires New 3DS" in UI. Audio-only fallback already works |

---

## 13. Open Questions

1. **Use ThirdTube's pre-built FFmpeg libs directly, or build from source?**
   Pro: saves a session of build work. Con: their build only has `mov` demuxer, we need `mpegts`.

2. **Keep mpg123 for audio-only mode, or replace entirely with FFmpeg AAC?**
   Recommendation: keep both. mpg123 for music streaming (lighter), FFmpeg AAC for video mode.

3. **Progressive TS vs HLS for MVP?**
   Recommendation: progressive TS. Simpler, one HTTP request, reuses ring buffer. Add HLS later for seeking.

4. **Should we detect New 3DS at runtime and disable video menu items on Old 3DS?**
   Yes. Use `OS_VersionBin` or check if `mvdstdInit` succeeds.

5. **What video bitrate is the sweet spot?**
   Tested 472kbps video + 128kbps audio = 600kbps total. Film content (2001) compressed to ~178kbps actual. Action content will be higher. 472kbps should be safe for most content.

---

## 14. References

### Repositories

| Repo | What We Use From It |
|---|---|
| [ThirdTube](https://github.com/windows-server-2003/ThirdTube) | FFmpeg build config, fake_pthread, MVD decode, A/V sync, threading model |
| [ThirdTube's FFmpeg fork](https://github.com/windows-server-2003/FFmpeg) | Patched FFmpeg source (pthreads, enum fixes) |
| [Video_player_for_3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS) | MVD API usage, AVCC→Annex B, SPS/PPS extraction, sentinel detection |
| [Switchfin](https://github.com/dragonflylee/switchfin) | Jellyfin DeviceProfile, PlaybackInfo flow, HLS transcoding params |
| [devkitPro libctru](https://github.com/devkitPro/libctru) | MVD service headers (`services/mvd.h`) |
| [devkitPro 3ds-examples](https://github.com/devkitPro/3ds-examples) | MVD example (`mvd/source/main.c`) |

### Documentation

| Resource | URL |
|---|---|
| 3dbrew MVD Services | https://www.3dbrew.org/wiki/MVD_Services |
| Jellyfin API (OpenAPI) | https://api.jellyfin.org/openapi/jellyfin-openapi-stable.json |
| Jellyfin API Overview | https://jmshrv.com/posts/jellyfin-api/ |
| Jellyfin Auth Gist | https://gist.github.com/nielsvanvelzen/ea047d9028f676185832e51ffaf12a6f |

### Verified Jellyfin Endpoints

```bash
# Progressive TS stream (MVP)
GET /Videos/{id}/stream.ts?VideoCodec=h264&AudioCodec=aac&VideoBitrate=472000&AudioBitrate=128000&MaxWidth=400&MaxHeight=240&Profile=Baseline&Level=31&TranscodingMaxAudioChannels=2&MediaSourceId={id}&api_key={key}

# HLS master playlist (future)
GET /Videos/{id}/master.m3u8?VideoCodec=h264&AudioCodec=aac&VideoBitrate=472000&AudioBitrate=128000&MaxWidth=400&MaxHeight=240&SegmentContainer=ts&SegmentLength=6&Profile=Baseline&Level=31&TranscodingMaxAudioChannels=2&MediaSourceId={id}&api_key={key}

# Subtitle stream
GET /Videos/{id}/{mediaSourceId}/Subtitles/{index}/0/Stream.srt?api_key={key}
```
