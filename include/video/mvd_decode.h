/**
 * mvd_decode.h — MVD hardware H.264 decoder (New 3DS only)
 */

#ifndef JFIN_MVD_DECODE_H
#define JFIN_MVD_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* MVD state */
    bool     initialized;
    int      width;          /* aligned to 16 */
    int      height;         /* aligned to 16 */

    /* Buffers (linear memory) */
    uint8_t *nal_buf;        /* input NAL unit buffer */
    int      nal_buf_size;
    uint8_t *frame_buf[2];   /* double-buffered output (BGR565) */
    int      frame_write_idx;

    /* SPS/PPS sent flag */
    bool     sps_sent;

    /* Frame ready flag */
    volatile bool frame_ready;
} mvd_ctx_t;

/**
 * Check if MVD hardware is available (New 3DS only).
 */
bool mvd_is_available(void);

/**
 * Initialize MVD for a given resolution.
 * Width/height will be aligned to 16-pixel boundaries.
 */
bool mvd_init(mvd_ctx_t *ctx, int width, int height);

/**
 * Send SPS and PPS extracted from FFmpeg extradata.
 * Must be called before the first video frame.
 */
bool mvd_send_sps_pps(mvd_ctx_t *ctx, const uint8_t *extradata, int extradata_size);

/**
 * Decode a video packet (AVCC format from FFmpeg).
 * Converts AVCC → Annex B internally and feeds to MVD.
 * Returns true if a decoded frame is ready in the output buffer.
 */
bool mvd_decode_packet(mvd_ctx_t *ctx, const uint8_t *data, int size);

/**
 * Get pointer to the current decoded frame (BGR565).
 * Only valid after mvd_decode_packet returns true.
 */
uint8_t *mvd_get_frame(mvd_ctx_t *ctx);

/**
 * Get frame dimensions (may differ from input due to alignment).
 */
void mvd_get_dimensions(const mvd_ctx_t *ctx, int *width, int *height);

/**
 * Shut down MVD and free all buffers.
 */
void mvd_cleanup(mvd_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_MVD_DECODE_H */
