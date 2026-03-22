# jellyfin-3ds Roadmap

## Vision

The first native Jellyfin client for Nintendo 3DS. Audio-first, with video as a stretch goal.

## Team

- **You** — project lead, decisions, 3DS hardware for testing
- **Claude** — implementation via multi-agent workflows (code, research, validation)
- **Dev 2** (on leave) — joins for polish phase, has second 3DS for testing

## Workflow: Human + Multi-Agent

```
You (direction + decisions + hardware testing)
 └─→ Claude session
      ├─→ Agent: implement feature (code generation)
      ├─→ Agent: validate build (compile checks in devkitPro Docker)
      ├─→ Agent: review code (security, correctness, 3DS pitfalls)
      └─→ Agent: research (API docs, reference repos)
```

### Validation Strategy

1. **Compile checks** — devkitPro Docker build must succeed, zero warnings
2. **API integration tests** — test Jellyfin API logic via curl against your real server
3. **Reference cross-check** — compare patterns against ThirdTube/Switchfin/ctrmus
4. **Citra smoke test** — UI layout and navigation (networking won't work in emulator)
5. **Real hardware** — you deploy via 3dslink, test networking + audio end-to-end

## Milestones

### M0: Build Infrastructure
- [ ] devkitPro Docker image for reproducible builds
- [ ] CI: `make` in Docker, produce .3dsx artifact
- [ ] Vendor cJSON into the repo
- [ ] Compile clean with zero warnings
- [ ] Validate Jellyfin API client logic against real server (curl tests from Proxmox)

### M1: Connect & Browse
- [ ] Login to Jellyfin server (username/password via swkbd)
- [ ] Fetch and display library views on bottom screen
- [ ] Navigate into libraries, albums, folders
- [ ] Paginated item lists (scroll past 50 items)
- [ ] Save session to SD card, restore on next boot
- [ ] Error handling: connection refused, bad credentials, timeout
- [ ] Verify on real 3DS via 3dslink

### M2: Audio Playback — **MVP GATE**
- [ ] Stream MP3 from Jellyfin /Audio/{id}/universal endpoint
- [ ] mpg123 decode → NDSP playback
- [ ] Now-playing screen with track info and progress bar
- [ ] Pause / resume / stop
- [ ] Report playback state to Jellyfin server
- [ ] Queue: play next track in album automatically
- [ ] Buffer underrun recovery (rebuffer without crash)
- [ ] End-to-end test: browse → play → hear audio

### M3: Polish (Dev 2 rejoins)
- [ ] Album art loading (JPEG decode → GPU texture)
- [ ] Touch scrolling with momentum
- [ ] Search via swkbd
- [ ] QuickConnect auth
- [ ] "Continue Listening" / recently added
- [ ] Seek within track
- [ ] Opus/Vorbis direct play
- [ ] Settings screen

### M4: Video Playback — Stretch
- [ ] H.264 Baseline transcode (400x240)
- [ ] MVD hardware decode (New 3DS)
- [ ] MJPEG software fallback (Old 3DS)
- [ ] Video top screen, controls bottom screen

## Non-Goals (for now)

- Live TV / DVR
- SyncPlay
- Stereoscopic 3D video
- Old 3DS video (audio-only is fine)
- Offline download / sync

## Technical Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| Single tester until Dev 2 returns | Hardware bugs found late | Validate thoroughly in code review + compile checks before each deploy |
| TLS handshake too slow | Login takes 5+ seconds | Connection pooling, HTTP fallback for LAN |
| WiFi bandwidth insufficient | Buffering/stuttering | Server transcoding at 128kbps, large prefetch buffer |
| Memory pressure from album art | OOM on Old 3DS | Lazy load, 3 textures max, downscale to 128x128 |
| mpg123 decode too slow | Audio stuttering | 128kbps MP3, or switch to tremor |
| Citra networking broken | Can't test network path in emulator | Test API layer via curl from Proxmox, trust ThirdTube's proven patterns |
