# Contributing to jellyfin-3ds

## Project Organization

We use a simple milestone-based workflow. No heavy methodology — just clear ownership and a shared roadmap.

### Structure

```
docs/
  ROADMAP.md         Milestones and task checklists (source of truth)
  CONTRIBUTING.md    This file
  ARCHITECTURE.md    Technical design decisions and module boundaries
```

### Workflow

1. **Pick a task** from the current milestone in ROADMAP.md
2. **Branch** from `main`: `feat/audio-playback`, `fix/login-timeout`, etc.
3. **Build & test** on real hardware (Citra for UI-only changes)
4. **PR** with a short description of what changed and how to test
5. **Check off** the task in ROADMAP.md when merged

### Commit Format

```
type: brief summary
```

Types: `feat`, `fix`, `docs`, `refactor`, `chore`, `build`

No emojis. No long bodies unless explaining a non-obvious decision.

### Branch Naming

```
feat/<short-description>    New functionality
fix/<short-description>     Bug fix
build/<short-description>   Build system / CI
docs/<short-description>    Documentation only
```

### Code Style

- C11 (not C++) for all core modules — keeps binary small, matches libctru
- 4-space indent, no tabs
- `snake_case` for functions and variables
- `UPPER_CASE` for constants and macros
- Prefix all public symbols: `jfin_` (API), `audio_player_` (audio), `ui_` (UI), `config_` (config)
- Keep functions short. If it doesn't fit on a screen, split it.
- Comments explain *why*, not *what*

### Module Ownership

Each module has a clear API boundary (header in `include/`). Modules communicate through these headers only — no reaching into another module's internals.

| Module | Directory | Depends On |
|--------|-----------|------------|
| API client | `src/api/` | libcurl, cJSON |
| Audio player | `src/audio/` | libcurl, mpg123, libctru (NDSP) |
| UI | `src/ui/` | citro2d, api (types only), audio (status only) |
| Config | `src/util/` | libctru (filesystem) |
| Main | `src/main.c` | All modules |

### Testing Strategy

- **Citra emulator**: UI layout, input handling, navigation logic
- **Real 3DS**: Networking, audio playback, performance
- **Deploy**: `3dslink jellyfin-3ds.3dsx` over WiFi
- **Debug output**: `printf()` goes to 3dslink console on the host PC

### Build Prerequisites

```bash
# Install devkitPro: https://devkitpro.org/wiki/Getting_Started
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib \
    3ds-libmpg123 3ds-libopus 3ds-opusfile 3ds-libvorbisidec 3ds-libogg

# Vendor cJSON
curl -L https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h -o include/api/cJSON.h
curl -L https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c -o src/api/cJSON.c
```
