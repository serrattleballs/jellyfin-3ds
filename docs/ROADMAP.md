# jellyfin-3ds Roadmap

## Vision

The first native Jellyfin client for Nintendo 3DS — audio and video.

## Status (as of 2026-03-23)

**Working prototype on real hardware.** Audio streaming (music + TV/movie), video streaming (H.264 MVD hardware decode on New 3DS), library browsing, login, session persistence.

### Completed

- [x] M0: Build infrastructure (devkitPro Docker, cJSON, CI)
- [x] M1: Connect & browse (login, library navigation, swkbd input)
- [x] M2: Audio playback (MP3 streaming, NDSP, pause/resume, 44.1/48kHz auto-detect)
- [x] M4: Video playback (FFmpeg TS demux, MVD H.264 decode, AAC audio, citro2d display)

### What Works

| Feature | Status | Notes |
|---|---|---|
| Login (user/pass) | Working | swkbd keyboard input |
| Session persistence | Working | saves to SD card on clean exit (START) |
| Library browsing | Working | music, movies, TV, all item types |
| Music streaming | Working | MP3 via mpg123, 44.1/48kHz |
| Movie/TV audio | Working | AAC via FFmpeg, 48kHz |
| Video playback | Working | H.264 Baseline 400x224, MVD hardware decode |
| Video + audio sync | Working | naturally synced via decode thread |
| Playback controls | Working | pause/resume/stop, progress bar |
| Playback reporting | Working | reports to Jellyfin dashboard |

### Known Issues

| Issue | Severity | Notes |
|---|---|---|
| Video pacing not perfectly smooth | Medium | No audio-master sync — plays at decode rate |
| ~30s initial buffer for video | Medium | Server transcoding startup + 128KB prefetch |
| Content without media files fails silently | Low | Shows "Demux init failed" — needs friendlier message |
| No auto-play next track/episode | Medium | Stops after each item |
| No credential caching via 3dslink | Low | Works when launching from SD card |
| Morton tiling CPU-intensive | Low | Could use DMA copy instead |

## Next Milestones

### M5: Stability & Error Handling
- [ ] Graceful error for content without media files (show message, return to browse)
- [ ] Better buffering UX (show "Server preparing stream..." during transcode startup)
- [ ] Handle network disconnects without crash
- [ ] Prevent double-play (stop current before starting new)
- [ ] Credential caching: verify token on startup, re-auth if expired
- [ ] Clean up video player resources on all exit paths

### M6: Audio-Visual Polish
- [ ] Audio-master A/V sync (video waits for audio PTS, frame skip when behind)
- [ ] Auto-play next track in album / next episode in series
- [ ] Album art loading (JPEG decode → GPU texture on now-playing screen)
- [ ] Reduce video startup time (smaller prefetch, probe_size hint)
- [ ] Search via swkbd
- [ ] "Continue Watching/Listening" on home screen

### M7: User Experience
- [ ] Settings screen (server URL, video bitrate, audio quality)
- [ ] QuickConnect auth (no keyboard needed)
- [ ] Touch scrolling with momentum on bottom screen
- [ ] Loading spinner / activity indicators
- [ ] New 3DS detection — hide video options on Old 3DS
- [ ] Volume control

### M8: Release Prep
- [ ] Custom app icon
- [ ] First-run setup wizard
- [ ] GBATemp release thread
- [ ] Universal-Updater listing
- [ ] User documentation

## Non-Goals (for now)

- Live TV / DVR
- SyncPlay (multi-device sync)
- Stereoscopic 3D video
- Old 3DS video playback (audio-only fallback works)
- Offline download / sync
- Subtitle rendering (burn-in via server works)
- HLS adaptive streaming
