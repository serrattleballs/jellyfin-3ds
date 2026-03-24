/**
 * player.h - Audio streaming player
 *
 * Streams audio from a URL, decodes to PCM16, and plays via NDSP.
 * Uses a background thread for network fetch + decode, and double-buffered
 * NDSP wave buffers for gapless playback.
 */

#ifndef JFIN_AUDIO_PLAYER_H
#define JFIN_AUDIO_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE    44100
#define AUDIO_CHANNELS       2
#define AUDIO_BUFFER_SAMPLES 4096  /* per buffer, ~93ms at 44.1kHz */
#define AUDIO_NUM_BUFFERS    4     /* ring buffer count */
#define AUDIO_PREFETCH_BYTES (256 * 1024) /* 256KB network prefetch */

typedef enum {
    PLAYER_STOPPED,
    PLAYER_LOADING,     /* buffering initial data */
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_ERROR
} player_state_t;

typedef struct {
    player_state_t state;
    int64_t  position_ticks;  /* current position (Jellyfin 10M ticks) */
    int64_t  duration_ticks;  /* total duration */
    int      buffer_percent;  /* network buffer fill 0-100 */
    char     error_msg[128];
} player_status_t;

/**
 * Initialize the audio player subsystem.
 * Call once at startup after ndspInit().
 */
bool audio_player_init(void);

/**
 * Shut down the audio player. Stops playback and frees resources.
 */
void audio_player_cleanup(void);

/**
 * Start playing audio from a stream URL.
 * Begins buffering immediately, playback starts when buffer is sufficient.
 */
bool audio_player_play(const char *url, int64_t duration_ticks, int64_t seek_offset_ticks);

/**
 * Stop playback and discard buffers.
 */
void audio_player_stop(void);

/**
 * Toggle pause/resume.
 */
void audio_player_pause(void);

/**
 * Seek to a position (in Jellyfin ticks, 10,000,000 = 1 second).
 * Requires re-buffering from the network.
 */
bool audio_player_seek(int64_t position_ticks);

/**
 * Get current player status. Safe to call from any thread.
 */
player_status_t audio_player_get_status(void);

/**
 * Must be called each frame from the main loop to pump NDSP buffers.
 */
void audio_player_update(void);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_AUDIO_PLAYER_H */
