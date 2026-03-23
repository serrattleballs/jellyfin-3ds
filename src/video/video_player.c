/**
 * video_player.c — Video streaming player orchestrator
 *
 * Coordinates: network download → FFmpeg demux → MVD decode → display
 *              + FFmpeg AAC decode → swr_convert → NDSP audio
 *
 * Threading: network thread fills ring buffer, decode thread demuxes
 * and feeds both MVD (video) and NDSP (audio), main thread renders.
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <curl/curl.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include "video/video_player.h"
#include "video/ffmpeg_demux.h"
#include "video/mvd_decode.h"

/* ── Ring buffer for network data ──────────────────────────────────── */

#define RING_SIZE       (512 * 1024)  /* 512KB */
#define PREFETCH_BYTES  (128 * 1024)  /* 128KB before starting decode */
#define AUDIO_BUF_SIZE  4096          /* PCM samples per NDSP buffer */
#define NUM_AUDIO_BUFS  4

/* ── Player state ──────────────────────────────────────────────────── */

static struct {
    /* Threads */
    Thread          net_thread;
    Thread          decode_thread;
    volatile bool   running;
    volatile bool   stop_requested;

    /* State */
    video_state_t   state;
    LightLock       state_lock;
    char            error_msg[128];

    /* Stream info */
    char            url[2048];
    int64_t         duration_ticks;
    volatile int64_t position_ticks;

    /* Demuxer */
    demux_ctx_t     demux;

    /* Video decode */
    mvd_ctx_t       mvd;
    bool            first_frame;

    /* Audio decode */
    AVCodecContext  *audio_dec_ctx;
    SwrContext      *swr_ctx;
    int             audio_sample_rate;

    /* NDSP audio output */
    int             ndsp_channel;
    ndspWaveBuf     wave_bufs[NUM_AUDIO_BUFS];
    s16            *pcm_bufs[NUM_AUDIO_BUFS];
    int             audio_buf_idx;

    /* Frame display */
    C3D_Tex         frame_tex;
    C2D_Image       frame_img;
    bool            tex_initialized;
    volatile bool   new_frame_available;
    LightLock       frame_lock;

    /* Display dimensions (actual video, may be smaller than tex) */
    int             display_width;
    int             display_height;
} s_vp;

/* ── Network thread ────────────────────────────────────────────────── */

static size_t net_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    demux_ctx_t *demux = (demux_ctx_t *)userdata;
    size_t total = size * nmemb;

    if (s_vp.stop_requested) return 0;

    size_t written = 0;
    while (written < total && !s_vp.stop_requested) {
        int space = demux->ring_size - demux->ring_fill;
        int chunk = (int)((total - written) < (size_t)space ? (total - written) : (size_t)space);

        if (chunk <= 0) {
            svcSleepThread(1000000LL); /* 1ms */
            continue;
        }

        for (int i = 0; i < chunk; i++) {
            demux->ring_data[demux->ring_write_pos] = ((uint8_t *)ptr)[written + i];
            demux->ring_write_pos = (demux->ring_write_pos + 1) % demux->ring_size;
        }
        demux->ring_fill += chunk;
        written += chunk;
    }

    return written;
}

static void net_thread_func(void *arg)
{
    (void)arg;
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "curl init failed");
        s_vp.state = VIDEO_ERROR;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, s_vp.url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, net_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s_vp.demux);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Jellyfin-3DS/0.1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK && !s_vp.stop_requested) {
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg),
                 "Stream: %s", curl_easy_strerror(res));
        s_vp.state = VIDEO_ERROR;
    }

    s_vp.demux.ring_finished = true;
    curl_easy_cleanup(curl);
}

/* ── Audio decode + output ─────────────────────────────────────────── */

static bool init_audio_decoder(demux_ctx_t *demux)
{
    if (demux->audio_stream_idx < 0)
        return false;

    AVFormatContext *fmt = (AVFormatContext *)demux->fmt_ctx;
    AVCodecParameters *par = fmt->streams[demux->audio_stream_idx]->codecpar;

    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;

    s_vp.audio_dec_ctx = avcodec_alloc_context3(codec);
    if (!s_vp.audio_dec_ctx) return false;

    avcodec_parameters_to_context(s_vp.audio_dec_ctx, par);
    if (avcodec_open2(s_vp.audio_dec_ctx, codec, NULL) < 0)
        return false;

    /* Set up resampler: whatever input format → s16 stereo */
    s_vp.swr_ctx = swr_alloc();
    if (!s_vp.swr_ctx) return false;

    s_vp.audio_sample_rate = par->sample_rate ? par->sample_rate : 48000;

    int64_t in_layout = s_vp.audio_dec_ctx->channel_layout;
    if (!in_layout)
        in_layout = av_get_default_channel_layout(s_vp.audio_dec_ctx->channels);

    av_opt_set_channel_layout(s_vp.swr_ctx, "in_channel_layout", in_layout, 0);
    av_opt_set_int(s_vp.swr_ctx, "in_sample_rate", s_vp.audio_dec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(s_vp.swr_ctx, "in_sample_fmt", s_vp.audio_dec_ctx->sample_fmt, 0);

    av_opt_set_channel_layout(s_vp.swr_ctx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_int(s_vp.swr_ctx, "out_sample_rate", s_vp.audio_sample_rate, 0);
    av_opt_set_sample_fmt(s_vp.swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(s_vp.swr_ctx) < 0)
        return false;

    /* NDSP channel for video audio */
    s_vp.ndsp_channel = 1; /* channel 0 is used by audio player */
    ndspChnSetInterp(s_vp.ndsp_channel, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(s_vp.ndsp_channel, (float)s_vp.audio_sample_rate);
    ndspChnSetFormat(s_vp.ndsp_channel, NDSP_FORMAT_STEREO_PCM16);

    float mix[12] = {0};
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(s_vp.ndsp_channel, mix);

    /* Allocate PCM buffers in linear memory */
    for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
        s_vp.pcm_bufs[i] = linearAlloc(AUDIO_BUF_SIZE * sizeof(s16) * 2);
        if (!s_vp.pcm_bufs[i]) return false;
        memset(&s_vp.wave_bufs[i], 0, sizeof(ndspWaveBuf));
        s_vp.wave_bufs[i].status = NDSP_WBUF_DONE;
    }

    return true;
}

static void decode_audio_packet(AVPacket *pkt)
{
    if (!s_vp.audio_dec_ctx) return;

    AVFrame *frame = av_frame_alloc();
    if (!frame) return;

    int ret = avcodec_send_packet(s_vp.audio_dec_ctx, pkt);
    if (ret < 0) { av_frame_free(&frame); return; }

    while (avcodec_receive_frame(s_vp.audio_dec_ctx, frame) >= 0) {
        /* Find a free NDSP buffer */
        ndspWaveBuf *wbuf = &s_vp.wave_bufs[s_vp.audio_buf_idx];

        /* Wait for buffer to be free */
        int waited = 0;
        while ((wbuf->status == NDSP_WBUF_QUEUED || wbuf->status == NDSP_WBUF_PLAYING)
               && !s_vp.stop_requested && waited < 1000) {
            svcSleepThread(1000000LL);
            waited++;
        }
        if (s_vp.stop_requested) break;

        /* Convert to s16 stereo */
        int out_samples = swr_convert(s_vp.swr_ctx,
            (uint8_t **)&s_vp.pcm_bufs[s_vp.audio_buf_idx], AUDIO_BUF_SIZE,
            (const uint8_t **)frame->extended_data, frame->nb_samples);

        if (out_samples > 0) {
            wbuf->data_vaddr = s_vp.pcm_bufs[s_vp.audio_buf_idx];
            wbuf->nsamples = out_samples;
            DSP_FlushDataCache(s_vp.pcm_bufs[s_vp.audio_buf_idx],
                               out_samples * sizeof(s16) * 2);
            ndspChnWaveBufAdd(s_vp.ndsp_channel, wbuf);

            s_vp.audio_buf_idx = (s_vp.audio_buf_idx + 1) % NUM_AUDIO_BUFS;
        }
    }

    av_frame_free(&frame);
}

/* ── Decode thread ─────────────────────────────────────────────────── */

static void decode_thread_func(void *arg)
{
    (void)arg;

    /* Wait for prefetch */
    while (!s_vp.stop_requested) {
        if (s_vp.demux.ring_fill >= PREFETCH_BYTES || s_vp.demux.ring_finished)
            break;
        svcSleepThread(10000000LL); /* 10ms */
    }
    if (s_vp.stop_requested) return;

    /* Init demuxer */
    if (!demux_init(&s_vp.demux)) {
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "Demux init failed");
        s_vp.state = VIDEO_ERROR;
        return;
    }

    /* Init MVD */
    if (!mvd_init(&s_vp.mvd, s_vp.demux.video_width, s_vp.demux.video_height)) {
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "MVD init failed (New 3DS?)");
        s_vp.state = VIDEO_ERROR;
        demux_cleanup(&s_vp.demux);
        return;
    }

    s_vp.display_width = s_vp.demux.video_width;
    s_vp.display_height = s_vp.demux.video_height;

    /* Send SPS/PPS */
    if (s_vp.demux.video_extradata) {
        mvd_send_sps_pps(&s_vp.mvd, s_vp.demux.video_extradata,
                          s_vp.demux.video_extradata_size);
    }

    /* Init audio decoder */
    init_audio_decoder(&s_vp.demux);

    LightLock_Lock(&s_vp.state_lock);
    s_vp.state = VIDEO_PLAYING;
    LightLock_Unlock(&s_vp.state_lock);

    s_vp.first_frame = true;

    /* Main decode loop */
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return;

    while (!s_vp.stop_requested) {
        bool is_video = false;
        int ret = demux_read_packet(&s_vp.demux, pkt, &is_video);
        if (ret < 0) break; /* EOF or error */

        if (is_video) {
            bool got_frame = mvd_decode_packet(&s_vp.mvd, pkt->data, pkt->size);

            if (got_frame) {
                if (s_vp.first_frame) {
                    /* Discard first frame (may be stale from previous session) */
                    s_vp.first_frame = false;
                } else {
                    LightLock_Lock(&s_vp.frame_lock);
                    s_vp.new_frame_available = true;
                    LightLock_Unlock(&s_vp.frame_lock);
                }
            }

            /* Update position from video PTS */
            if (pkt->pts != AV_NOPTS_VALUE) {
                AVFormatContext *fmt = (AVFormatContext *)s_vp.demux.fmt_ctx;
                AVRational tb = fmt->streams[s_vp.demux.video_stream_idx]->time_base;
                double sec = pkt->pts * (double)tb.num / (double)tb.den;
                s_vp.position_ticks = (int64_t)(sec * 10000000.0);
            }
        } else {
            decode_audio_packet(pkt);
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);

    if (!s_vp.stop_requested) {
        LightLock_Lock(&s_vp.state_lock);
        s_vp.state = VIDEO_STOPPED;
        LightLock_Unlock(&s_vp.state_lock);
    }
}

/* ── GPU texture upload ────────────────────────────────────────────── */

static void init_frame_texture(int width, int height)
{
    /* citro3d textures must be power-of-two dimensions */
    int tex_w = 512; /* >= 400 */
    int tex_h = 256; /* >= 240 */

    if (!C3D_TexInit(&s_vp.frame_tex, tex_w, tex_h, GPU_RGB565))
        return;

    C3D_TexSetFilter(&s_vp.frame_tex, GPU_LINEAR, GPU_LINEAR);

    /* Set up C2D_Image to render a sub-region of the texture */
    s_vp.frame_img.tex = &s_vp.frame_tex;
    s_vp.frame_img.subtex = &(Tex3DS_SubTexture){
        .width = (u16)width,
        .height = (u16)height,
        .left = 0.0f,
        .top = 1.0f,
        .right = (float)width / tex_w,
        .bottom = 1.0f - ((float)height / tex_h),
    };

    s_vp.tex_initialized = true;
}

/* Morton/Z-order swizzle for 3DS GPU textures.
 * The GPU stores textures in 8x8 Morton-tiled blocks. */
static inline uint32_t morton_interleave(uint32_t x, uint32_t y)
{
    static const uint32_t mortonTable[8] = {
        0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15
    };
    return mortonTable[x % 8] + mortonTable[y % 8] * 2;
}

static void upload_bgr565_to_texture(const uint8_t *src, int src_w, int src_h)
{
    if (!s_vp.tex_initialized) return;

    uint16_t *tex_data = (uint16_t *)s_vp.frame_tex.data;
    const uint16_t *pixels = (const uint16_t *)src;
    int tex_w = s_vp.frame_tex.width;
    int tex_h = s_vp.frame_tex.height;

    /* Copy with Morton tiling */
    for (int y = 0; y < src_h && y < tex_h; y++) {
        for (int x = 0; x < src_w && x < tex_w; x++) {
            /* Calculate Morton index */
            int block_x = x / 8;
            int block_y = y / 8;
            int block_idx = block_x + block_y * (tex_w / 8);
            int morton_off = morton_interleave(x, y);
            int tex_idx = block_idx * 64 + morton_off;

            tex_data[tex_idx] = pixels[y * src_w + x];
        }
    }

    C3D_TexFlush(&s_vp.frame_tex);
}

/* ── Public API ────────────────────────────────────────────────────── */

bool video_player_is_supported(void)
{
    return mvd_is_available();
}

bool video_player_init(void)
{
    memset(&s_vp, 0, sizeof(s_vp));
    LightLock_Init(&s_vp.state_lock);
    LightLock_Init(&s_vp.frame_lock);
    s_vp.state = VIDEO_STOPPED;
    return true;
}

void video_player_cleanup(void)
{
    video_player_stop();
}

bool video_player_play(const char *url, int64_t duration_ticks)
{
    video_player_stop();

    snprintf(s_vp.url, sizeof(s_vp.url), "%s", url);
    s_vp.duration_ticks = duration_ticks;
    s_vp.position_ticks = 0;
    s_vp.error_msg[0] = '\0';
    s_vp.stop_requested = false;
    s_vp.state = VIDEO_LOADING;
    s_vp.new_frame_available = false;
    s_vp.audio_buf_idx = 0;

    /* Allocate ring buffer */
    s_vp.demux.ring_data = malloc(RING_SIZE);
    if (!s_vp.demux.ring_data) return false;
    s_vp.demux.ring_size = RING_SIZE;
    s_vp.demux.ring_write_pos = 0;
    s_vp.demux.ring_read_pos = 0;
    s_vp.demux.ring_fill = 0;
    s_vp.demux.ring_finished = false;

    /* Reset NDSP channel */
    ndspChnReset(1);

    /* Launch threads */
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    s_vp.net_thread = threadCreate(net_thread_func, NULL,
                                    32 * 1024, prio - 1, 1, false);
    s_vp.decode_thread = threadCreate(decode_thread_func, NULL,
                                       64 * 1024, prio - 1, 2, false);

    if (!s_vp.net_thread || !s_vp.decode_thread) {
        s_vp.state = VIDEO_ERROR;
        return false;
    }

    return true;
}

void video_player_stop(void)
{
    if (s_vp.state == VIDEO_STOPPED && !s_vp.net_thread)
        return;

    s_vp.stop_requested = true;

    if (s_vp.net_thread) {
        threadJoin(s_vp.net_thread, U64_MAX);
        threadFree(s_vp.net_thread);
        s_vp.net_thread = NULL;
    }
    if (s_vp.decode_thread) {
        threadJoin(s_vp.decode_thread, U64_MAX);
        threadFree(s_vp.decode_thread);
        s_vp.decode_thread = NULL;
    }

    ndspChnWaveBufClear(s_vp.ndsp_channel);

    /* Clean up audio decoder */
    if (s_vp.swr_ctx) {
        swr_free(&s_vp.swr_ctx);
        s_vp.swr_ctx = NULL;
    }
    if (s_vp.audio_dec_ctx) {
        avcodec_free_context(&s_vp.audio_dec_ctx);
        s_vp.audio_dec_ctx = NULL;
    }
    for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
        if (s_vp.pcm_bufs[i]) {
            linearFree(s_vp.pcm_bufs[i]);
            s_vp.pcm_bufs[i] = NULL;
        }
    }

    mvd_cleanup(&s_vp.mvd);
    demux_cleanup(&s_vp.demux);

    if (s_vp.demux.ring_data) {
        free(s_vp.demux.ring_data);
        s_vp.demux.ring_data = NULL;
    }

    if (s_vp.tex_initialized) {
        C3D_TexDelete(&s_vp.frame_tex);
        s_vp.tex_initialized = false;
    }

    s_vp.state = VIDEO_STOPPED;
}

void video_player_pause(void)
{
    LightLock_Lock(&s_vp.state_lock);
    if (s_vp.state == VIDEO_PLAYING) {
        ndspChnSetPaused(s_vp.ndsp_channel, true);
        s_vp.state = VIDEO_PAUSED;
    } else if (s_vp.state == VIDEO_PAUSED) {
        ndspChnSetPaused(s_vp.ndsp_channel, false);
        s_vp.state = VIDEO_PLAYING;
    }
    LightLock_Unlock(&s_vp.state_lock);
}

video_status_t video_player_get_status(void)
{
    video_status_t st;
    st.state = s_vp.state;
    st.position_ticks = s_vp.position_ticks;
    st.duration_ticks = s_vp.duration_ticks;
    st.buffer_percent = (s_vp.demux.ring_size > 0)
        ? (s_vp.demux.ring_fill * 100 / s_vp.demux.ring_size) : 0;
    st.video_width = s_vp.display_width;
    st.video_height = s_vp.display_height;
    st.fps = 0; /* TODO */
    snprintf(st.error_msg, sizeof(st.error_msg), "%s", s_vp.error_msg);
    return st;
}

void video_player_render_frame(void)
{
    if (s_vp.state != VIDEO_PLAYING && s_vp.state != VIDEO_PAUSED)
        return;

    /* Check if decode thread produced a new frame */
    LightLock_Lock(&s_vp.frame_lock);
    bool has_frame = s_vp.new_frame_available;
    s_vp.new_frame_available = false;
    LightLock_Unlock(&s_vp.frame_lock);

    if (has_frame) {
        /* Init texture on first frame */
        if (!s_vp.tex_initialized)
            init_frame_texture(s_vp.display_width, s_vp.display_height);

        /* Upload decoded BGR565 frame to GPU texture */
        uint8_t *frame = mvd_get_frame(&s_vp.mvd);
        if (frame)
            upload_bgr565_to_texture(frame, s_vp.mvd.width, s_vp.mvd.height);
    }

    /* Draw the frame texture on the top screen */
    if (s_vp.tex_initialized) {
        /* Center video on top screen (400x240) */
        float x = (400 - s_vp.display_width) / 2.0f;
        float y = (240 - s_vp.display_height) / 2.0f;
        C2D_DrawImageAt(s_vp.frame_img, x, y, 0.5f, NULL, 1.0f, 1.0f);
    }
}
