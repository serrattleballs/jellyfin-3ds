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

#include "util/log.h"
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
#define PREFETCH_BYTES  (32 * 1024)   /* 32KB before starting decode */
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
    bool            audio_swr_ready;
    SwrContext      *swr_ctx;
    int             audio_sample_rate;

    /* A/V sync — audio is the master clock */
    double          audio_buf_pts[NUM_AUDIO_BUFS]; /* PTS (seconds) per queued buffer */
    volatile bool   audio_playing;

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
    volatile double frame_pts;       /* PTS of the pending frame (seconds) */
    LightLock       frame_lock;

    /* Display dimensions (actual video, may be smaller than tex) */
    int             display_width;
    int             display_height;

    /* Diagnostics */
    int             frames_decoded;
    int             frames_displayed;
    int             frames_skipped;
    u64             last_fps_tick;
    float           decode_fps;
    float           display_fps;
} s_vp;

/* ── Audio clock (for A/V sync) ────────────────────────────────────── */

/**
 * Get current audio playback position in seconds.
 * Returns -1.0 if audio is not playing (video should display immediately).
 * Same approach as ThirdTube/FourthTube's Util_speaker_get_current_timestamp.
 */
static double get_audio_clock(void)
{
    if (!s_vp.audio_playing || s_vp.audio_sample_rate <= 0)
        return -1.0;

    /* Find the currently playing buffer */
    for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
        if (s_vp.wave_bufs[i].status == NDSP_WBUF_PLAYING) {
            double sample_offset = (double)ndspChnGetSamplePos(s_vp.ndsp_channel)
                                 / (double)s_vp.audio_sample_rate;
            return s_vp.audio_buf_pts[i] + sample_offset;
        }
    }

    /* No buffer playing — check if any are queued */
    double min_queued = 1e30;
    for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
        if (s_vp.wave_bufs[i].status == NDSP_WBUF_QUEUED) {
            if (s_vp.audio_buf_pts[i] < min_queued)
                min_queued = s_vp.audio_buf_pts[i];
        }
    }
    if (min_queued < 1e29)
        return min_queued;

    return -1.0;
}

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
    log_write("NET: thread started");
    CURL *curl = curl_easy_init();
    if (!curl) {
        log_write("NET: curl_easy_init failed");
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "curl init failed");
        s_vp.state = VIDEO_ERROR;
        return;
    }
    log_write("NET: fetching URL (len=%d)", (int)strlen(s_vp.url));

    curl_easy_setopt(curl, CURLOPT_URL, s_vp.url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, net_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s_vp.demux);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Jellyfin-3DS/0.1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    log_write("NET: curl done, result=%d (%s), http=%ld, ring_fill=%d",
              res, curl_easy_strerror(res), http_code, s_vp.demux.ring_fill);

    if (res != CURLE_OK && !s_vp.stop_requested) {
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg),
                 "HTTP %ld: %s", http_code, curl_easy_strerror(res));
        s_vp.state = VIDEO_ERROR;
    }

    s_vp.demux.ring_finished = true;
    curl_easy_cleanup(curl);
}

/* ── Audio decode + output ─────────────────────────────────────────── */

static bool init_audio_decoder(demux_ctx_t *demux)
{
    if (demux->audio_stream_idx < 0) {
        log_write("AUDIO: no audio stream found");
        return false;
    }

    AVFormatContext *fmt = (AVFormatContext *)demux->fmt_ctx;
    AVCodecParameters *par = fmt->streams[demux->audio_stream_idx]->codecpar;

    log_write("AUDIO: codec_id=%d sample_rate=%d channels=%d",
              par->codec_id, par->sample_rate, par->channels);

    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        log_write("AUDIO: avcodec_find_decoder FAILED for codec_id=%d", par->codec_id);
        return false;
    }
    log_write("AUDIO: found decoder '%s'", codec->name);

    s_vp.audio_dec_ctx = avcodec_alloc_context3(codec);
    if (!s_vp.audio_dec_ctx) {
        log_write("AUDIO: avcodec_alloc_context3 FAILED");
        return false;
    }

    avcodec_parameters_to_context(s_vp.audio_dec_ctx, par);

    /* TS demuxer may report 0 for sample_rate/channels until first decode.
     * Default to Jellyfin's transcode output: 48kHz stereo AAC. */
    if (s_vp.audio_dec_ctx->sample_rate == 0)
        s_vp.audio_dec_ctx->sample_rate = 48000;
    if (s_vp.audio_dec_ctx->channels == 0) {
        s_vp.audio_dec_ctx->channels = 2;
        s_vp.audio_dec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
    }

    int open_ret = avcodec_open2(s_vp.audio_dec_ctx, codec, NULL);
    if (open_ret < 0) {
        log_write("AUDIO: avcodec_open2 FAILED ret=%d", open_ret);
        return false;
    }
    log_write("AUDIO: decoder opened (swr deferred to first frame)");

    /* swr init is deferred to decode_audio_packet — the actual sample format
     * isn't known until after the first frame is decoded (TS quirk). */
    s_vp.audio_swr_ready = false;
    s_vp.audio_sample_rate = 48000; /* default, updated on first frame */

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
        /* Deferred swr init: now we know the real format from the decoded frame */
        if (!s_vp.audio_swr_ready) {
            if (s_vp.swr_ctx) swr_free(&s_vp.swr_ctx);
            s_vp.swr_ctx = swr_alloc();
            if (!s_vp.swr_ctx) break;

            int64_t in_layout = frame->channel_layout;
            if (!in_layout)
                in_layout = av_get_default_channel_layout(frame->channels);
            int in_rate = frame->sample_rate ? frame->sample_rate : 48000;

            av_opt_set_channel_layout(s_vp.swr_ctx, "in_channel_layout", in_layout, 0);
            av_opt_set_int(s_vp.swr_ctx, "in_sample_rate", in_rate, 0);
            av_opt_set_sample_fmt(s_vp.swr_ctx, "in_sample_fmt", frame->format, 0);

            av_opt_set_channel_layout(s_vp.swr_ctx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
            av_opt_set_int(s_vp.swr_ctx, "out_sample_rate", in_rate, 0);
            av_opt_set_sample_fmt(s_vp.swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

            if (swr_init(s_vp.swr_ctx) < 0) {
                log_write("AUDIO: swr_init FAILED (deferred) fmt=%d rate=%d ch=%d",
                          frame->format, in_rate, frame->channels);
                break;
            }

            s_vp.audio_sample_rate = in_rate;
            ndspChnSetRate(s_vp.ndsp_channel, (float)in_rate);
            s_vp.audio_swr_ready = true;
            log_write("AUDIO: swr init OK (deferred) fmt=%d rate=%d ch=%d",
                      frame->format, in_rate, frame->channels);
        }

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
            /* Store PTS for A/V sync before queuing */
            double frame_pts = -1.0;
            if (frame->pts != AV_NOPTS_VALUE && s_vp.demux.fmt_ctx) {
                AVFormatContext *fmt = (AVFormatContext *)s_vp.demux.fmt_ctx;
                AVRational tb = fmt->streams[s_vp.demux.audio_stream_idx]->time_base;
                frame_pts = frame->pts * (double)tb.num / (double)tb.den;
            }
            s_vp.audio_buf_pts[s_vp.audio_buf_idx] = frame_pts;

            wbuf->data_vaddr = s_vp.pcm_bufs[s_vp.audio_buf_idx];
            wbuf->nsamples = out_samples;
            DSP_FlushDataCache(s_vp.pcm_bufs[s_vp.audio_buf_idx],
                               out_samples * sizeof(s16) * 2);
            ndspChnWaveBufAdd(s_vp.ndsp_channel, wbuf);
            s_vp.audio_playing = true;

            s_vp.audio_buf_idx = (s_vp.audio_buf_idx + 1) % NUM_AUDIO_BUFS;
        }
    }

    av_frame_free(&frame);
}

/* ── Decode thread ─────────────────────────────────────────────────── */

static void decode_thread_func(void *arg)
{
    (void)arg;
    log_write("DEC: thread started, waiting for prefetch (%d bytes)", PREFETCH_BYTES);

    /* Wait for prefetch */
    while (!s_vp.stop_requested) {
        if (s_vp.demux.ring_fill >= PREFETCH_BYTES || s_vp.demux.ring_finished)
            break;
        svcSleepThread(10000000LL); /* 10ms */
    }
    if (s_vp.stop_requested) { log_write("DEC: stop during prefetch"); return; }

    log_write("DEC: prefetch done, fill=%d finished=%d", s_vp.demux.ring_fill, s_vp.demux.ring_finished);

    /* Init demuxer */
    if (!demux_init(&s_vp.demux)) {
        log_write("DEC: demux_init FAILED");
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "Demux init failed");
        s_vp.state = VIDEO_ERROR;
        return;
    }
    log_write("DEC: demux OK video=%dx%d vidx=%d aidx=%d",
              s_vp.demux.video_width, s_vp.demux.video_height,
              s_vp.demux.video_stream_idx, s_vp.demux.audio_stream_idx);

    /* Init MVD */
    if (!mvd_init(&s_vp.mvd, s_vp.demux.video_width, s_vp.demux.video_height)) {
        log_write("DEC: mvd_init FAILED");
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "MVD init failed (New 3DS?)");
        s_vp.state = VIDEO_ERROR;
        demux_cleanup(&s_vp.demux);
        return;
    }
    log_write("DEC: MVD OK %dx%d", s_vp.mvd.width, s_vp.mvd.height);

    s_vp.display_width = s_vp.demux.video_width;
    s_vp.display_height = s_vp.demux.video_height;

    /* Send SPS/PPS (only for AVCC/MP4 — TS has them inline) */
    if (s_vp.demux.video_extradata && s_vp.demux.video_extradata_size > 0) {
        log_write("DEC: sending SPS/PPS from extradata (%d bytes)", s_vp.demux.video_extradata_size);
        mvd_send_sps_pps(&s_vp.mvd, s_vp.demux.video_extradata,
                          s_vp.demux.video_extradata_size);
    } else {
        log_write("DEC: no extradata — TS stream, SPS/PPS inline in NAL units");
        s_vp.mvd.sps_sent = true; /* skip the gate in decode_packet */
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

            /* Compute video PTS in seconds */
            double video_pts = -1.0;
            if (pkt->pts != AV_NOPTS_VALUE) {
                AVFormatContext *fmt = (AVFormatContext *)s_vp.demux.fmt_ctx;
                AVRational tb = fmt->streams[s_vp.demux.video_stream_idx]->time_base;
                video_pts = pkt->pts * (double)tb.num / (double)tb.den;
                s_vp.position_ticks = (int64_t)(video_pts * 10000000.0);
            }

            if (got_frame) {
                if (s_vp.first_frame) {
                    s_vp.first_frame = false;
                } else {
                    /* Post frame for display — never block.
                     * If the previous frame hasn't been consumed, overwrite it.
                     * Audio keeps flowing because we never wait here. */
                    LightLock_Lock(&s_vp.frame_lock);
                    s_vp.new_frame_available = true;
                    s_vp.frame_pts = video_pts;
                    s_vp.frames_decoded++;
                    LightLock_Unlock(&s_vp.frame_lock);
                }
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

/* Static subtex so it persists (compound literals on stack would dangle) */
static Tex3DS_SubTexture s_subtex;

static void init_frame_texture(int width, int height)
{
    /* citro3d textures must be power-of-two dimensions */
    int tex_w = 512; /* >= 400 */
    int tex_h = 256; /* >= 240 */

    if (!C3D_TexInit(&s_vp.frame_tex, tex_w, tex_h, GPU_RGB565)) {
        log_write("TEX: C3D_TexInit FAILED %dx%d", tex_w, tex_h);
        return;
    }

    C3D_TexSetFilter(&s_vp.frame_tex, GPU_LINEAR, GPU_LINEAR);

    /* Set up C2D_Image to render a sub-region of the texture */
    s_subtex.width = (u16)width;
    s_subtex.height = (u16)height;
    s_subtex.left = 0.0f;
    s_subtex.top = 1.0f;
    s_subtex.right = (float)width / tex_w;
    s_subtex.bottom = 1.0f - ((float)height / tex_h);

    s_vp.frame_img.tex = &s_vp.frame_tex;
    s_vp.frame_img.subtex = &s_subtex;

    s_vp.tex_initialized = true;
    log_write("TEX: init OK %dx%d in %dx%d tex", width, height, tex_w, tex_h);
}

/* GPU-accelerated texture upload via GX_DisplayTransfer.
 * The GPU DMA handles Morton tiling in hardware — zero CPU cost.
 * Source must be in linear memory (our MVD output already is). */

static void upload_bgr565_to_texture(const uint8_t *src, int src_w, int src_h)
{
    if (!s_vp.tex_initialized) return;

    int tex_w = s_vp.frame_tex.width;
    int tex_h = s_vp.frame_tex.height;

    /* Flush source data from CPU cache so GPU DMA sees it */
    GSPGPU_FlushDataCache(src, src_w * src_h * 2);

    /* GX_DisplayTransfer: GPU DMA that converts linear → tiled.
     * GX_TRANSFER_OUT_TILED(1) enables Morton tiling on output.
     * GX_TRANSFER_FLIP_VERT(1) because 3DS textures are bottom-up. */
    C3D_SyncDisplayTransfer(
        (u32 *)src,
        GX_BUFFER_DIM(src_w, src_h),
        (u32 *)s_vp.frame_tex.data,
        GX_BUFFER_DIM(tex_w, tex_h),
        (GX_TRANSFER_FLIP_VERT(1) |
         GX_TRANSFER_OUT_TILED(1) |
         GX_TRANSFER_RAW_COPY(0) |
         GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
         GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
         GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
    );
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
    log_write("PLAY: starting video, url_len=%d", (int)strlen(url));
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

    /* Launch threads (-1 = any available core) */
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    log_write("PLAY: creating threads, prio=%ld", (long)prio);

    s_vp.net_thread = threadCreate(net_thread_func, NULL,
                                    32 * 1024, prio - 1, -1, false);
    s_vp.decode_thread = threadCreate(decode_thread_func, NULL,
                                       64 * 1024, prio - 1, -1, false);

    if (!s_vp.net_thread || !s_vp.decode_thread) {
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg),
                 "Thread create failed (net=%p dec=%p)",
                 (void*)s_vp.net_thread, (void*)s_vp.decode_thread);
        /* Clean up any thread that DID start */
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
        free(s_vp.demux.ring_data);
        s_vp.demux.ring_data = NULL;
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
    /* Compute FPS every second */
    u64 now = svcGetSystemTick();
    u64 elapsed = now - s_vp.last_fps_tick;
    if (elapsed > SYSCLOCK_ARM11) { /* 1 second */
        double sec = (double)elapsed / (double)SYSCLOCK_ARM11;
        s_vp.decode_fps = s_vp.frames_decoded / sec;
        s_vp.display_fps = s_vp.frames_displayed / sec;
        s_vp.frames_decoded = 0;
        s_vp.frames_displayed = 0;
        s_vp.last_fps_tick = now;
    }
    st.decode_fps = s_vp.decode_fps;
    st.display_fps = s_vp.display_fps;
    st.frames_decoded = s_vp.frames_decoded;
    st.frames_displayed = s_vp.frames_displayed;
    snprintf(st.error_msg, sizeof(st.error_msg), "%s", s_vp.error_msg);
    return st;
}

void video_player_render_frame(void)
{
    static int render_log_count = 0;

    if (s_vp.state != VIDEO_PLAYING && s_vp.state != VIDEO_PAUSED)
        return;

    /* Check if decode thread produced a new frame */
    LightLock_Lock(&s_vp.frame_lock);
    bool has_frame = s_vp.new_frame_available;
    double pending_pts = s_vp.frame_pts;
    LightLock_Unlock(&s_vp.frame_lock);

    if (has_frame) {
        /* A/V sync check: is it time to show this frame?
         * If video PTS is ahead of audio by >5ms, keep waiting (don't consume).
         * If audio isn't playing, show immediately. */
        bool should_display = true;
        if (pending_pts >= 0) {
            double audio_pos = get_audio_clock();
            if (audio_pos >= 0 && pending_pts - audio_pos > 0.005) {
                should_display = false; /* not yet — wait for next vsync */
            }
        }

        if (should_display) {
            /* Consume the frame */
            LightLock_Lock(&s_vp.frame_lock);
            s_vp.new_frame_available = false;
            LightLock_Unlock(&s_vp.frame_lock);

            render_log_count++;

            if (!s_vp.tex_initialized)
                init_frame_texture(s_vp.display_width, s_vp.display_height);

            uint8_t *frame = mvd_get_frame(&s_vp.mvd);
            if (frame) {
                upload_bgr565_to_texture(frame, s_vp.mvd.width, s_vp.mvd.height);
                s_vp.frames_displayed++;
                if (render_log_count <= 3) {
                    log_write("RENDER: frame#%d pts=%.3f uploaded %dx%d",
                              render_log_count, pending_pts, s_vp.mvd.width, s_vp.mvd.height);
                }
            }
        }
    }

    /* Draw the frame texture on the top screen */
    if (s_vp.tex_initialized) {
        float x = (400 - s_vp.display_width) / 2.0f;
        float y = (240 - s_vp.display_height) / 2.0f;
        C2D_DrawImageAt(s_vp.frame_img, x, y, 0.5f, NULL, 1.0f, 1.0f);
    }
}
