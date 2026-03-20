# jellyfin-3ds Roadmap

## Vision

The first native Jellyfin client for Nintendo 3DS. Audio-first, with video as a stretch goal.

## Milestones

### M0: Build Infrastructure (Week 1)
- [ ] devkitPro Docker image for reproducible builds
- [ ] CI pipeline (GitHub Actions: build on push, produce .3dsx artifact)
- [ ] Vendor cJSON into the repo
- [ ] Verify blank app boots on real 3DS hardware via 3dslink
- [ ] Verify blank app renders citro2d text on both screens in Citra

### M1: Connect & Browse (Week 1-2)
- [ ] Login to Jellyfin server (username/password via swkbd)
- [ ] Fetch and display library views on bottom screen
- [ ] Navigate into libraries, albums, folders
- [ ] Paginated item lists (scroll past 50 items)
- [ ] Save session to SD card, restore on next boot
- [ ] Error handling: connection refused, bad credentials, timeout

### M2: Audio Playback (Week 2-4) — **MVP GATE**
- [ ] Stream MP3 from Jellyfin /Audio/{id}/universal endpoint
- [ ] mpg123 decode → NDSP playback on real hardware
- [ ] Now-playing screen with track info and progress bar
- [ ] Pause / resume / stop
- [ ] Report playback state to Jellyfin server
- [ ] Queue: play next track in album automatically
- [ ] Buffer underrun recovery (rebuffer without crash)

### M3: Polish (Week 4-6)
- [ ] Album art loading and display (JPEG decode → GPU texture)
- [ ] Touch scrolling with momentum on bottom screen
- [ ] Search (swkbd input → search results)
- [ ] QuickConnect auth (no keyboard needed)
- [ ] "Continue Listening" / recently added on home screen
- [ ] Seek within a track (re-request stream with startTimeTicks)
- [ ] Opus/Vorbis direct play (skip server transcode for supported formats)
- [ ] Settings screen (server URL, bitrate, transcode preference)

### M4: Video Playback (Week 6-10) — Stretch
- [ ] H.264 Baseline transcode request (400x240, low bitrate)
- [ ] MVD hardware decode on New 3DS
- [ ] MJPEG software fallback on Old 3DS
- [ ] Video on top screen, transport controls on bottom
- [ ] Movie/episode browsing with series/season hierarchy

### M5: Release (Week 10+)
- [ ] Icon and banner assets
- [ ] First-run setup wizard
- [ ] GBATemp release thread
- [ ] Universal-Updater listing
- [ ] Documentation: supported formats, known limitations, FAQ

## Non-Goals (for now)

- Live TV / DVR
- SyncPlay (multi-device sync)
- Stereoscopic 3D video
- Old 3DS video (audio-only on Old 3DS is fine)
- Offline download / sync

## Technical Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| TLS handshake too slow | Login takes 5+ seconds | Connection pooling, HTTP fallback for LAN |
| WiFi bandwidth insufficient for video | Stuttering, buffering | Aggressive server transcoding (400kbps), large prefetch buffer |
| Memory pressure from album art | OOM crash on Old 3DS | Lazy load, max 3 textures cached, downscale to 128x128 |
| mpg123 decode too slow | Audio stuttering | Use hardware-friendly bitrate (128kbps), or switch to tremor (vorbis) |
| MVD hardware decoder quirks | Video artifacts, crashes | Reference Video_player_for_3DS implementation carefully |
| Citra networking broken | Can't test without hardware | Accept this — develop UI in Citra, test networking on real 3DS |
