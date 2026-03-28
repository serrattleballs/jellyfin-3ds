/**
 * mvd_decode.c — MVD hardware H.264 decoder (New 3DS only)
 *
 * Feeds AVCC-format H.264 packets from FFmpeg's demuxer to the
 * MVD hardware decoder after converting to Annex B format.
 *
 * Reference: Video_player_for_3DS/source/system/util/decoder.c
 * Reference: ThirdTube/source/network_decoder/network_decoder.cpp
 */

#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "video/mvd_decode.h"
#include "util/log.h"

#define ALIGN16(x) (((x) + 15) & ~15)
#define NAL_BUF_SIZE (256 * 1024)  /* 256KB for NAL unit conversion */

/* ── Availability check ────────────────────────────────────────────── */

bool mvd_is_available(void)
{
    /* Try to init MVD — only succeeds on New 3DS */
    Result ret = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264,
                            MVD_OUTPUT_BGR565, MVD_DEFAULT_WORKBUF_SIZE, NULL);
    if (R_SUCCEEDED(ret)) {
        mvdstdExit();
        return true;
    }
    return false;
}

/* ── Init / Cleanup ────────────────────────────────────────────────── */

bool mvd_init(mvd_ctx_t *ctx, int width, int height)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->width = ALIGN16(width);
    ctx->height = ALIGN16(height);

    /* Initialize MVD service */
    Result ret = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264,
                            MVD_OUTPUT_BGR565, MVD_DEFAULT_WORKBUF_SIZE, NULL);
    if (R_FAILED(ret))
        return false;

    /* Allocate NAL buffer in linear memory (required by MVD DMA) */
    ctx->nal_buf = linearAlloc(NAL_BUF_SIZE);
    if (!ctx->nal_buf) {
        mvdstdExit();
        return false;
    }
    ctx->nal_buf_size = NAL_BUF_SIZE;

    /* Allocate double-buffered output frames in linear memory */
    int frame_size = ctx->width * ctx->height * 2; /* BGR565 = 2 bytes/pixel */
    for (int i = 0; i < 2; i++) {
        ctx->frame_buf[i] = linearAlloc(frame_size);
        if (!ctx->frame_buf[i]) {
            mvd_cleanup(ctx);
            return false;
        }
        memset(ctx->frame_buf[i], 0, frame_size);
    }

    ctx->frame_write_idx = 0;
    ctx->initialized = true;
    return true;
}

void mvd_cleanup(mvd_ctx_t *ctx)
{
    if (ctx->initialized)
        mvdstdExit();

    if (ctx->nal_buf) {
        linearFree(ctx->nal_buf);
        ctx->nal_buf = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (ctx->frame_buf[i]) {
            linearFree(ctx->frame_buf[i]);
            ctx->frame_buf[i] = NULL;
        }
    }
    ctx->initialized = false;
}

/* ── SPS/PPS extraction from AVCC extradata ────────────────────────── */

bool mvd_send_sps_pps(mvd_ctx_t *ctx, const uint8_t *extradata, int extradata_size)
{
    if (!extradata || extradata_size < 8)
        return false;

    /* AVCC extradata layout:
     * [0]    version
     * [1-3]  profile, compat, level
     * [4]    length_size_minus_one (lower 2 bits)
     * [5]    num_sps (lower 5 bits)
     * [6-7]  sps_length (big-endian)
     * [8..]  sps_data
     * [..]   num_pps
     * [..]   pps_length
     * [..]   pps_data
     */

    int offset = 6;
    int sps_len = (extradata[offset] << 8) | extradata[offset + 1];
    offset += 2;

    if (offset + sps_len > extradata_size)
        return false;

    /* Send SPS with Annex B start code */
    int pos = 0;
    ctx->nal_buf[pos++] = 0x00;
    ctx->nal_buf[pos++] = 0x00;
    ctx->nal_buf[pos++] = 0x01;
    memcpy(ctx->nal_buf + pos, extradata + offset, sps_len);
    pos += sps_len;

    GSPGPU_FlushDataCache(ctx->nal_buf, pos);
    mvdstdProcessVideoFrame(ctx->nal_buf, pos, 0, NULL);

    offset += sps_len;

    /* PPS count byte */
    if (offset >= extradata_size) return false;
    offset++; /* skip num_pps */

    if (offset + 2 > extradata_size) return false;
    int pps_len = (extradata[offset] << 8) | extradata[offset + 1];
    offset += 2;

    if (offset + pps_len > extradata_size)
        return false;

    /* Send PPS with Annex B start code */
    pos = 0;
    ctx->nal_buf[pos++] = 0x00;
    ctx->nal_buf[pos++] = 0x00;
    ctx->nal_buf[pos++] = 0x01;
    memcpy(ctx->nal_buf + pos, extradata + offset, pps_len);
    pos += pps_len;

    GSPGPU_FlushDataCache(ctx->nal_buf, pos);
    mvdstdProcessVideoFrame(ctx->nal_buf, pos, 0, NULL);

    ctx->sps_sent = true;
    return true;
}

/* ── Decode a video packet ─────────────────────────────────────────── */

bool mvd_decode_packet(mvd_ctx_t *ctx, const uint8_t *data, int size)
{
    if (!ctx->initialized)
        return false;

    /* Detect packet format:
     * - TS demuxer: already Annex B (starts with 00 00 00 01 or 00 00 01)
     * - MP4 demuxer: AVCC format (4-byte big-endian length prefix)
     * Check by looking for Annex B start code at the beginning. */
    bool is_annexb = (size >= 4 &&
                      data[0] == 0x00 && data[1] == 0x00 &&
                      (data[2] == 0x01 || (data[2] == 0x00 && data[3] == 0x01)));

    int dst_off = 0;

    if (is_annexb) {
        /* Already Annex B — copy directly to linear memory buffer.
         * SPS/PPS are inline in the stream, MVD handles them automatically. */
        if (size > ctx->nal_buf_size)
            size = ctx->nal_buf_size;
        memcpy(ctx->nal_buf, data, size);
        dst_off = size;
    } else {
        /* AVCC format — convert to Annex B */
        int src_off = 0;
        while (src_off + 4 < size && dst_off + 3 < ctx->nal_buf_size) {
            uint32_t nal_size = ((uint32_t)data[src_off] << 24) |
                                ((uint32_t)data[src_off + 1] << 16) |
                                ((uint32_t)data[src_off + 2] << 8) |
                                (uint32_t)data[src_off + 3];
            src_off += 4;

            if ((int)(src_off + nal_size) > size)
                break;
            if ((int)(dst_off + 3 + nal_size) > ctx->nal_buf_size)
                break;

            ctx->nal_buf[dst_off++] = 0x00;
            ctx->nal_buf[dst_off++] = 0x00;
            ctx->nal_buf[dst_off++] = 0x01;

            memcpy(ctx->nal_buf + dst_off, data + src_off, nal_size);
            dst_off += nal_size;
            src_off += nal_size;
        }
    }

    if (dst_off == 0)
        return false;

    /* Sanity check: reject obviously corrupt NAL data before feeding MVD.
     * MVD operates via DMA — corrupt input can crash the entire system. */
    if (dst_off < 5) {
        log_write("MVD: NAL too small (%d bytes), skipping", dst_off);
        return false;
    }

    /* Set output config for the current write buffer */
    MVDSTD_Config config;
    mvdstdGenerateDefaultConfig(&config,
                                 ctx->width, ctx->height,
                                 ctx->width, ctx->height,
                                 NULL, NULL, NULL);
    config.physaddr_outdata0 = osConvertVirtToPhys(ctx->frame_buf[ctx->frame_write_idx]);

    MVDSTD_SetConfig(&config);

    /* Flush cache and feed NAL to MVD */
    GSPGPU_FlushDataCache(ctx->nal_buf, dst_off);
    Result ret = mvdstdProcessVideoFrame(ctx->nal_buf, dst_off, 0, NULL);

    static int frame_count = 0;
    frame_count++;
    if (frame_count <= 5 || frame_count % 100 == 0) {
        log_write("MVD: ProcessNAL ret=0x%08lX nal_size=%d frame#%d",
                  (unsigned long)ret, dst_off, frame_count);
    }

    if (R_FAILED(ret) && !MVD_CHECKNALUPROC_SUCCESS(ret)) {
        log_write("MVD: ProcessNAL FAILED ret=0x%08lX", (unsigned long)ret);
        return false;
    }

    /* Try to render the decoded frame */
    if (ret != MVD_STATUS_PARAMSET) {
        ret = mvdstdRenderVideoFrame(&config, true);
        if (frame_count <= 5 || frame_count % 100 == 0) {
            log_write("MVD: RenderFrame ret=0x%08lX", (unsigned long)ret);
        }
        if (R_SUCCEEDED(ret) || ret == MVD_STATUS_OK) {
            GSPGPU_FlushDataCache(ctx->frame_buf[ctx->frame_write_idx],
                                   ctx->width * ctx->height * 2);
            ctx->frame_write_idx ^= 1; /* swap double buffer */
            ctx->frame_ready = true;
            return true;
        }
    }

    return false;
}

uint8_t *mvd_get_frame(mvd_ctx_t *ctx)
{
    /* Return the last completed frame (not the one being written to) */
    return ctx->frame_buf[ctx->frame_write_idx ^ 1];
}

void mvd_get_dimensions(const mvd_ctx_t *ctx, int *width, int *height)
{
    if (width) *width = ctx->width;
    if (height) *height = ctx->height;
}
