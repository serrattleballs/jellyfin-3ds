/**
 * album_art.c — Download, decode, and display album art from Jellyfin
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "ui/album_art.h"
#include "api/jellyfin.h"
#include "util/stb_image.h"
#include "util/log.h"

/* ── Download buffer ───────────────────────────────────────────────── */

typedef struct {
    u8    *data;
    size_t size;
    size_t capacity;
} dl_buf_t;

static size_t dl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    dl_buf_t *buf = (dl_buf_t *)userdata;
    size_t total = size * nmemb;

    if (buf->size + total >= buf->capacity) {
        size_t new_cap = (buf->capacity + total) * 2;
        if (new_cap > 512 * 1024) return 0; /* 512KB max for album art */
        u8 *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    return total;
}

/* ── State ─────────────────────────────────────────────────────────── */

#define ART_TEX_SIZE 128 /* power-of-two texture size */

static struct {
    C3D_Tex      tex;
    C2D_Image    img;
    Tex3DS_SubTexture subtex;
    bool         loaded;
    char         cached_id[64]; /* item ID of cached art */
    int          art_w, art_h;  /* actual image dimensions */
} s_art;

/* ── Morton tiling for art texture ─────────────────────────────────── */

static void upload_rgb565_art(const u16 *pixels, int w, int h)
{
    u8 *tex_data = (u8 *)s_art.tex.data;
    int tex_w = ART_TEX_SIZE;

    /* Precomputed Morton offset tables — values × pixel_size(2) */
    static int inc_x[ART_TEX_SIZE];
    static int inc_y[ART_TEX_SIZE];
    static bool tables_built = false;
    const int ps = 2; /* BGR565 = 2 bytes/pixel */

    if (!tables_built) {
        for (int i = 0; i + 3 < ART_TEX_SIZE; i += 4) {
            inc_x[i]     = 4 * ps;
            inc_x[i + 1] = 12 * ps;
            inc_x[i + 2] = 4 * ps;
            inc_x[i + 3] = 44 * ps;
        }
        for (int i = 0; i + 7 < ART_TEX_SIZE; i += 8) {
            inc_y[i]     = 2 * ps;
            inc_y[i + 1] = 6 * ps;
            inc_y[i + 2] = 2 * ps;
            inc_y[i + 3] = 22 * ps;
            inc_y[i + 4] = 2 * ps;
            inc_y[i + 5] = 6 * ps;
            inc_y[i + 6] = 2 * ps;
            inc_y[i + 7] = (tex_w * 8 - 42) * ps;
        }
        tables_built = true;
    }

    int dst_row = 0, y_count = 0;
    for (int y = 0; y < h && y < ART_TEX_SIZE; y++) {
        const u8 *row = (const u8 *)(pixels + y * w);
        int dst_pos = dst_row, x_count = 0;
        for (int x = 0; x < w && x_count < 64; x += 2) {
            *(u32 *)(tex_data + dst_pos) = *(const u32 *)(row + x * 2);
            dst_pos += inc_x[x_count++];
        }
        dst_row += inc_y[y_count++];
    }

    C3D_TexFlush(&s_art.tex);
}

/* ── Public API ────────────────────────────────────────────────────── */

bool album_art_load(const jfin_session_t *session, const jfin_item_t *item)
{
    /* Check cache — don't re-download for the same item */
    const char *art_id = item->id;
    if (item->has_album_image && item->album_id[0])
        art_id = item->album_id;
    else if (!item->has_primary_image && !item->has_album_image)
        return false; /* no art available */

    if (s_art.loaded && strcmp(s_art.cached_id, art_id) == 0)
        return true; /* already cached */

    /* Build image URL */
    char url[1024];
    jfin_get_image_url_for_item(session, item, ART_TEX_SIZE, ART_TEX_SIZE, url, sizeof(url));

    /* Download */
    dl_buf_t dl = { .data = malloc(32 * 1024), .size = 0, .capacity = 32 * 1024 };
    if (!dl.data) return false;

    CURL *curl = curl_easy_init();
    if (!curl) { free(dl.data); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dl);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || dl.size == 0) {
        free(dl.data);
        return false;
    }

    /* Decode JPEG/PNG */
    int w, h, channels;
    u8 *rgb = stbi_load_from_memory(dl.data, dl.size, &w, &h, &channels, 3);
    free(dl.data);

    if (!rgb) {
        log_write("ART: decode failed");
        return false;
    }

    /* Clamp to texture size */
    if (w > ART_TEX_SIZE) w = ART_TEX_SIZE;
    if (h > ART_TEX_SIZE) h = ART_TEX_SIZE;

    /* Convert RGB888 → RGB565 */
    u16 *rgb565 = malloc(w * h * 2);
    if (!rgb565) { stbi_image_free(rgb); return false; }

    for (int i = 0; i < w * h; i++) {
        u8 r = rgb[i * 3];
        u8 g = rgb[i * 3 + 1];
        u8 b = rgb[i * 3 + 2];
        /* RGB565: RRRRRGGGGGGBBBBB */
        rgb565[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    stbi_image_free(rgb);

    /* Init texture if needed */
    if (!s_art.loaded) {
        if (!C3D_TexInit(&s_art.tex, ART_TEX_SIZE, ART_TEX_SIZE, GPU_RGB565)) {
            free(rgb565);
            return false;
        }
        C3D_TexSetFilter(&s_art.tex, GPU_LINEAR, GPU_LINEAR);
        memset(s_art.tex.data, 0, ART_TEX_SIZE * ART_TEX_SIZE * 2);
    }

    /* Upload to texture */
    upload_rgb565_art(rgb565, w, h);
    free(rgb565);

    /* Set up C2D image */
    s_art.subtex.width = (u16)w;
    s_art.subtex.height = (u16)h;
    s_art.subtex.left = 0.0f;
    s_art.subtex.top = 1.0f;
    s_art.subtex.right = (float)w / ART_TEX_SIZE;
    s_art.subtex.bottom = 1.0f - ((float)h / ART_TEX_SIZE);

    s_art.img.tex = &s_art.tex;
    s_art.img.subtex = &s_art.subtex;
    s_art.art_w = w;
    s_art.art_h = h;
    s_art.loaded = true;
    snprintf(s_art.cached_id, sizeof(s_art.cached_id), "%s", art_id);

    log_write("ART: loaded %dx%d from %s", w, h, art_id);
    return true;
}

void album_art_draw(float x, float y, float size)
{
    if (!s_art.loaded) return;

    float scale = size / (float)s_art.art_w;
    float scale_y = size / (float)s_art.art_h;
    if (scale_y < scale) scale = scale_y; /* fit within box */

    float draw_w = s_art.art_w * scale;
    float draw_h = s_art.art_h * scale;
    float draw_x = x + (size - draw_w) / 2.0f;
    float draw_y = y + (size - draw_h) / 2.0f;

    C2D_DrawImageAt(s_art.img, draw_x, draw_y, 0.5f, NULL, scale, scale);
}

bool album_art_is_loaded(void)
{
    return s_art.loaded;
}

void album_art_cleanup(void)
{
    if (s_art.loaded) {
        C3D_TexDelete(&s_art.tex);
        s_art.loaded = false;
        s_art.cached_id[0] = '\0';
    }
}
