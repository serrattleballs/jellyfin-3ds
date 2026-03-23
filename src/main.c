/**
 * main.c - Jellyfin 3DS client entry point
 *
 * Initializes 3DS services, runs the main loop, and cleans up.
 * The main loop is a standard frame-based loop:
 *   1. Poll input
 *   2. Update UI state
 *   3. Update audio player (pump NDSP buffers)
 *   4. Render both screens
 */

#include <3ds.h>
#include <citro2d.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>

#include "api/jellyfin.h"
#include "audio/player.h"
#include "video/video_player.h"
#include "ui/ui.h"
#include "util/config.h"

/* Target 30fps — sufficient for a media browser UI */
#define TARGET_FPS 30

static jfin_session_t  s_session;
static jfin_config_t   s_config;
static ui_state_t      s_ui;

static void init_services(void)
{
    /* Core 3DS services */
    gfxInitDefault();
    gfxSet3D(false); /* no stereoscopic 3D needed */

    /* Networking */
    u32 *soc_buf = (u32 *)memalign(0x1000, 0x100000); /* 1MB socket buffer */
    socInit(soc_buf, 0x100000);

    /* Audio */
    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    /* GPU / 2D rendering */
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    /* Filesystem (for SD card config/cache) */
    /* romfsInit() if we have bundled assets */
}

static void cleanup_services(void)
{
    C2D_Fini();
    C3D_Fini();
    ndspExit();
    socExit();
    gfxExit();
}

static bool try_auto_login(void)
{
    if (s_config.server_url[0] == '\0' || s_config.access_token[0] == '\0')
        return false;

    /* Restore session from saved config */
    snprintf(s_session.server_url, sizeof(s_session.server_url), "%s", s_config.server_url);
    snprintf(s_session.access_token, sizeof(s_session.access_token), "%s", s_config.access_token);
    snprintf(s_session.user_id, sizeof(s_session.user_id), "%s", s_config.user_id);
    snprintf(s_session.device_id, sizeof(s_session.device_id), "%s", s_config.device_id);

    /* TODO: validate token with GET /Users/{id} — for now assume valid */
    s_session.authenticated = true;
    return true;
}

int main(int argc, char *argv[])
{
    init_services();

    /* Load config */
    config_load(&s_config);
    config_ensure_device_id(&s_config);

    /* Init subsystems — if any fail, exit gracefully */
    if (!jfin_init() || !audio_player_init() || !ui_init()) {
        cleanup_services();
        return 1;
    }
    video_player_init(); /* optional — fails gracefully on Old 3DS */

    /* Try restoring previous session */
    memset(&s_session, 0, sizeof(s_session));
    memset(&s_ui, 0, sizeof(s_ui));

    if (try_auto_login()) {
        s_ui.current_view = VIEW_LIBRARIES;
    } else {
        s_ui.current_view = VIEW_LOGIN;
        /* Pre-fill server URL from config if available */
        if (s_config.server_url[0] != '\0')
            snprintf(s_ui.server_url, sizeof(s_ui.server_url), "%s", s_config.server_url);
    }

    /* ── Main Loop ─────────────────────────────────────────────────── */

    while (aptMainLoop()) {
        /* Input */
        hidScanInput();
        u32 kdown = hidKeysDown();
        u32 kheld = hidKeysHeld();
        touchPosition touch;
        hidTouchRead(&touch);

        /* Exit on START */
        if (kdown & KEY_START)
            break;

        /* Update */
        ui_update(&s_ui, &s_session, kdown, kheld, touch);
        audio_player_update();

        /* Render */
        player_status_t pstatus = audio_player_get_status();
        ui_render(&s_ui, &s_session, &pstatus);

        /* Frame pacing handled by gspWaitForVBlank */
        gspWaitForVBlank();
    }

    /* ── Cleanup ───────────────────────────────────────────────────── */

    /* Save session for next launch */
    if (s_session.authenticated) {
        snprintf(s_config.access_token, sizeof(s_config.access_token), "%s", s_session.access_token);
        snprintf(s_config.user_id, sizeof(s_config.user_id), "%s", s_session.user_id);
        config_save(&s_config);
    }

    video_player_stop();
    video_player_cleanup();
    audio_player_stop();
    audio_player_cleanup();
    ui_cleanup();
    jfin_cleanup();
    cleanup_services();

    return 0;
}
