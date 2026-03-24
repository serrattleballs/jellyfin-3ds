/**
 * ui.c - Dual-screen UI implementation
 *
 * Uses citro2d for GPU-accelerated 2D rendering.
 * Top screen: now-playing / branding
 * Bottom screen: touch-driven list navigation
 *
 * MVP: text-based rendering. Album art loading deferred to phase 2.
 */

#include <stdio.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>

#include "ui/ui.h"
#include "api/jellyfin.h"
#include "audio/player.h"
#include "video/video_player.h"
#include "util/config.h"

extern jfin_config_t g_config;

/* ── Render targets ────────────────────────────────────────────────── */

static C3D_RenderTarget *s_top    = NULL;
static C3D_RenderTarget *s_bottom = NULL;
static C2D_TextBuf       s_text_buf = NULL;
static C2D_Font          s_font = NULL;

/* ── Helpers ───────────────────────────────────────────────────────── */

static u32 rgba(u32 hex)
{
    /* Convert 0xRRGGBBAA to citro2d's ABGR format */
    u8 r = (hex >> 24) & 0xFF;
    u8 g = (hex >> 16) & 0xFF;
    u8 b = (hex >> 8)  & 0xFF;
    u8 a = hex & 0xFF;
    return C2D_Color32(r, g, b, a);
}

static void draw_text(float x, float y, float size, u32 color, const char *text)
{
    C2D_Text c2d_text;
    C2D_TextParse(&c2d_text, s_text_buf, text);
    C2D_TextOptimize(&c2d_text);
    C2D_DrawText(&c2d_text, C2D_WithColor, x, y, 0.5f, size, size, color);
}

static void draw_rect(float x, float y, float w, float h, u32 color)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

static void format_ticks(int64_t ticks, char *out, int out_len)
{
    int total_sec = (int)(ticks / 10000000LL);
    int min = total_sec / 60;
    int sec = total_sec % 60;
    snprintf(out, out_len, "%d:%02d", min, sec);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

bool ui_init(void)
{
    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    if (!s_top || !s_bottom) return false;

    s_text_buf = C2D_TextBufNew(4096);
    if (!s_text_buf) return false;

    /* Use system font */
    s_font = NULL; /* NULL = default system font */

    return true;
}

void ui_cleanup(void)
{
    if (s_text_buf) {
        C2D_TextBufDelete(s_text_buf);
        s_text_buf = NULL;
    }
}

/* ── Input Handling ────────────────────────────────────────────────── */

void ui_update(ui_state_t *state, const jfin_session_t *session,
               u32 kdown, u32 kheld, touchPosition touch)
{
    (void)kheld;

    switch (state->current_view) {
    case VIEW_LOGIN:
        /* D-pad up/down to select field */
        if (kdown & KEY_DUP) {
            state->login_field = (state->login_field + 2) % 3;
        }
        if (kdown & KEY_DDOWN) {
            state->login_field = (state->login_field + 1) % 3;
        }
        /* A to activate swkbd for the selected field */
        if (kdown & KEY_A) {
            SwkbdState swkbd;
            char buf[JFIN_MAX_URL] = {0};

            SwkbdType type = (state->login_field == 2)
                ? SWKBD_TYPE_WESTERN : SWKBD_TYPE_WESTERN;
            swkbdInit(&swkbd, type, 2, 255);

            switch (state->login_field) {
            case 0:
                swkbdSetHintText(&swkbd, "Server URL (e.g. http://192.168.1.100:8096)");
                snprintf(buf, sizeof(buf), "%s", state->server_url);
                break;
            case 1:
                swkbdSetHintText(&swkbd, "Username");
                snprintf(buf, sizeof(buf), "%s", state->username);
                break;
            case 2:
                swkbdSetHintText(&swkbd, "Password");
                swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
                break;
            }

            swkbdSetInitialText(&swkbd, buf);
            SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));

            if (button == SWKBD_BUTTON_CONFIRM) {
                switch (state->login_field) {
                case 0: snprintf(state->server_url, sizeof(state->server_url), "%s", buf); break;
                case 1: snprintf(state->username, sizeof(state->username), "%s", buf); break;
                case 2: snprintf(state->password, sizeof(state->password), "%s", buf); break;
                }
            }
        }
        /* START to attempt login */
        if (kdown & KEY_R) {
            /* Show connecting indicator before blocking login call */
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TextBufClear(s_text_buf);
            C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
            C2D_SceneBegin(s_bottom);
            draw_text(100, 110, 0.6f, rgba(COLOR_PRIMARY), "Connecting...");
            C3D_FrameEnd(0);

            jfin_session_t *s = (jfin_session_t *)session; /* cast away const for login */
            if (jfin_login(s, state->server_url, state->username, state->password)) {
                /* Save credentials immediately so they persist even on crash */
                config_save_session(&g_config, s->server_url,
                                    s->access_token, s->user_id,
                                    state->username);
                state->current_view = VIEW_LIBRARIES;
                jfin_get_views(session, &state->items);
                state->selected_index = 0;
                state->scroll_offset = 0;
            }
        }
        break;

    case VIEW_LIBRARIES:
    case VIEW_BROWSE:
        /* D-pad navigation */
        if (kdown & KEY_DUP) {
            if (state->selected_index > 0) {
                state->selected_index--;
                if (state->selected_index < state->scroll_offset)
                    state->scroll_offset = state->selected_index;
            }
        }
        if (kdown & KEY_DDOWN) {
            if (state->selected_index < state->items.count - 1) {
                state->selected_index++;
                if (state->selected_index >= state->scroll_offset + UI_MAX_VISIBLE_ITEMS)
                    state->scroll_offset = state->selected_index - UI_MAX_VISIBLE_ITEMS + 1;
            }
        }
        /* A to enter / play */
        if (kdown & KEY_A) {
            if (state->selected_index < state->items.count) {
                jfin_item_t *item = &state->items.items[state->selected_index];
                bool is_playable = (item->type == JFIN_ITEM_AUDIO ||
                                    item->type == JFIN_ITEM_MOVIE ||
                                    item->type == JFIN_ITEM_EPISODE);
                bool is_container = (item->type == JFIN_ITEM_FOLDER ||
                                     item->type == JFIN_ITEM_MUSIC_ALBUM ||
                                     item->type == JFIN_ITEM_MUSIC_ARTIST ||
                                     item->type == JFIN_ITEM_SERIES ||
                                     item->type == JFIN_ITEM_SEASON);
                bool is_video = (item->type == JFIN_ITEM_MOVIE ||
                                 item->type == JFIN_ITEM_EPISODE);
                if (is_playable) {
                    if (is_video && video_player_is_supported()) {
                        /* Video playback on New 3DS */
                        audio_player_stop();
                        jfin_stream_t stream;
                        if (jfin_get_video_stream(session, item->id, &stream) &&
                            video_player_play(stream.url, item->runtime_ticks)) {
                            state->now_playing = *item;
                            state->has_now_playing = true;
                            state->playing_index = state->selected_index;
                            state->auto_stopped = false;
                            state->previous_view = state->current_view;
                            state->current_view = VIEW_NOW_PLAYING;
                            jfin_report_start(session, item->id);
                        } else {
                            /* Video failed — fall back to audio */
                            if (jfin_get_audio_stream(session, item->id, &stream)) {
                                audio_player_play(stream.url, item->runtime_ticks);
                                state->now_playing = *item;
                                state->has_now_playing = true;
                                state->playing_index = state->selected_index;
                                state->auto_stopped = false;
                                jfin_report_start(session, item->id);
                            }
                        }
                    } else {
                        /* Audio-only (music, or video on Old 3DS) */
                        video_player_stop(); /* stop any video playback */
                        jfin_stream_t stream;
                        if (jfin_get_audio_stream(session, item->id, &stream)) {
                            audio_player_play(stream.url, item->runtime_ticks);
                            state->now_playing = *item;
                            state->has_now_playing = true;
                            state->playing_index = state->selected_index;
                            state->auto_stopped = false;
                            jfin_report_start(session, item->id);
                        }
                    }
                } else if (is_container) {
                    ui_navigate_into(state, session, item);
                }
                /* JFIN_ITEM_UNKNOWN: do nothing */
            }
        }
        /* B to go back */
        if (kdown & KEY_B) {
            ui_navigate_back(state, session);
        }
        /* Touch: tap on list items */
        if (kdown & KEY_TOUCH) {
            int tapped = state->scroll_offset + (touch.py / UI_LIST_ITEM_HEIGHT);
            if (tapped < state->items.count) {
                state->selected_index = tapped;
                /* Double-tap could enter — for now just select */
            }
        }
        /* L/R for pagination */
        if (kdown & KEY_R) {
            /* Next page */
            if (state->items.start_index + state->items.count < state->items.total_count) {
                int next_start = state->items.start_index + JFIN_MAX_ITEMS;
                const char *parent_id = (state->parent_depth > 0)
                    ? state->parent_stack_ids[state->parent_depth - 1] : NULL;
                if (state->current_view == VIEW_LIBRARIES) {
                    /* Libraries don't paginate the same way */
                } else if (parent_id) {
                    jfin_get_items(session, parent_id, next_start, JFIN_MAX_ITEMS, &state->items);
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                }
            }
        }
        if (kdown & KEY_L) {
            /* Previous page */
            if (state->items.start_index > 0) {
                int prev_start = state->items.start_index - JFIN_MAX_ITEMS;
                if (prev_start < 0) prev_start = 0;
                const char *parent_id = (state->parent_depth > 0)
                    ? state->parent_stack_ids[state->parent_depth - 1] : NULL;
                if (parent_id) {
                    jfin_get_items(session, parent_id, prev_start, JFIN_MAX_ITEMS, &state->items);
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                }
            }
        }
        /* Y to toggle now-playing view */
        if ((kdown & KEY_Y) && state->has_now_playing) {
            state->previous_view = state->current_view;
            state->current_view = VIEW_NOW_PLAYING;
        }
        /* SELECT to search */
        if (kdown & KEY_SELECT) {
            SwkbdState swkbd;
            char query[128] = {0};
            swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 127);
            swkbdSetHintText(&swkbd, "Search...");
            SwkbdButton button = swkbdInputText(&swkbd, query, sizeof(query));
            if (button == SWKBD_BUTTON_CONFIRM && query[0] != '\0') {
                jfin_search(session, query, JFIN_MAX_ITEMS, &state->items);
                state->selected_index = 0;
                state->scroll_offset = 0;
                state->current_view = VIEW_BROWSE;
                /* Don't push to breadcrumb -- search results are a flat list */
            }
        }
        /* Auto-advance: when current track/episode finishes, play next */
        if (state->has_now_playing && state->auto_advance && !state->auto_stopped) {
            player_status_t ps = audio_player_get_status();
            video_status_t vs = video_player_get_status();
            bool audio_finished = (ps.state == PLAYER_STOPPED && vs.state == VIDEO_STOPPED);
            bool video_finished = (vs.state == VIDEO_STOPPED && ps.state == PLAYER_STOPPED);

            if (audio_finished || video_finished) {
                int next = state->playing_index + 1;
                if (next < state->items.count) {
                    jfin_item_t *next_item = &state->items.items[next];
                    /* Only auto-advance to same playable type */
                    bool next_playable = (next_item->type == JFIN_ITEM_AUDIO ||
                                          next_item->type == JFIN_ITEM_MOVIE ||
                                          next_item->type == JFIN_ITEM_EPISODE);
                    bool next_is_video = (next_item->type == JFIN_ITEM_MOVIE ||
                                          next_item->type == JFIN_ITEM_EPISODE);
                    if (next_playable) {
                        jfin_stream_t stream;
                        bool started = false;
                        if (next_is_video && video_player_is_supported()) {
                            if (jfin_get_video_stream(session, next_item->id, &stream) &&
                                video_player_play(stream.url, next_item->runtime_ticks)) {
                                started = true;
                            } else if (jfin_get_audio_stream(session, next_item->id, &stream)) {
                                audio_player_play(stream.url, next_item->runtime_ticks);
                                started = true;
                            }
                        } else {
                            if (jfin_get_audio_stream(session, next_item->id, &stream)) {
                                audio_player_play(stream.url, next_item->runtime_ticks);
                                started = true;
                            }
                        }
                        if (started) {
                            state->now_playing = *next_item;
                            state->playing_index = next;
                            state->auto_stopped = false;
                            jfin_report_start(session, next_item->id);
                        } else {
                            state->has_now_playing = false;
                        }
                    } else {
                        state->has_now_playing = false;
                    }
                } else {
                    state->has_now_playing = false;
                }
            }
        }
        break;

    case VIEW_NOW_PLAYING:
        {
            video_status_t vs = video_player_get_status();
            bool vid_active = (vs.state == VIDEO_PLAYING || vs.state == VIDEO_PAUSED ||
                               vs.state == VIDEO_LOADING);

            /* Error state: any button press stops and returns to browse */
            if (vs.state == VIDEO_ERROR) {
                if (kdown) {
                    video_player_stop();
                    audio_player_stop();
                    state->has_now_playing = false;
                    state->current_view = state->previous_view;
                }
                break;
            }

            /* A to pause/resume */
            if (kdown & KEY_A) {
                if (vid_active)
                    video_player_pause();
                else
                    audio_player_pause();
            }
            /* B to go back to browse */
            if (kdown & KEY_B) {
                state->current_view = state->previous_view;
            }
            /* X to stop */
            if (kdown & KEY_X) {
                video_player_stop();
                audio_player_stop();
                state->has_now_playing = false;
                state->auto_stopped = true;
                state->current_view = state->previous_view;
            }
            /* Auto-advance from now-playing screen */
            if (state->has_now_playing && state->auto_advance && !state->auto_stopped) {
                player_status_t ps = audio_player_get_status();
                bool audio_done = (ps.state == PLAYER_STOPPED && vs.state == VIDEO_STOPPED);
                bool video_done = (vs.state == VIDEO_STOPPED && ps.state == PLAYER_STOPPED);

                if (audio_done || video_done) {
                    int next = state->playing_index + 1;
                    if (next < state->items.count) {
                        jfin_item_t *next_item = &state->items.items[next];
                        bool next_playable = (next_item->type == JFIN_ITEM_AUDIO ||
                                              next_item->type == JFIN_ITEM_MOVIE ||
                                              next_item->type == JFIN_ITEM_EPISODE);
                        bool next_is_video = (next_item->type == JFIN_ITEM_MOVIE ||
                                              next_item->type == JFIN_ITEM_EPISODE);
                        if (next_playable) {
                            jfin_stream_t stream;
                            bool started = false;
                            if (next_is_video && video_player_is_supported()) {
                                if (jfin_get_video_stream(session, next_item->id, &stream) &&
                                    video_player_play(stream.url, next_item->runtime_ticks)) {
                                    started = true;
                                } else if (jfin_get_audio_stream(session, next_item->id, &stream)) {
                                    audio_player_play(stream.url, next_item->runtime_ticks);
                                    started = true;
                                }
                            } else {
                                if (jfin_get_audio_stream(session, next_item->id, &stream)) {
                                    audio_player_play(stream.url, next_item->runtime_ticks);
                                    started = true;
                                }
                            }
                            if (started) {
                                state->now_playing = *next_item;
                                state->playing_index = next;
                                state->auto_stopped = false;
                                jfin_report_start(session, next_item->id);
                            } else {
                                state->has_now_playing = false;
                                state->current_view = state->previous_view;
                            }
                        } else {
                            state->has_now_playing = false;
                            state->current_view = state->previous_view;
                        }
                    } else {
                        state->has_now_playing = false;
                        state->current_view = state->previous_view;
                    }
                }
            }
        }
        break;
    }
}

/* ── Navigation ────────────────────────────────────────────────────── */

void ui_navigate_into(ui_state_t *state, const jfin_session_t *session,
                      const jfin_item_t *item)
{
    /* Push breadcrumb first, then fetch. If empty, pop it back. */
    char saved_id[JFIN_MAX_ID];
    char saved_name[JFIN_MAX_NAME];
    snprintf(saved_id, sizeof(saved_id), "%s", item->id);
    snprintf(saved_name, sizeof(saved_name), "%s", item->name);

    if (state->parent_depth < 8) {
        snprintf(state->parent_stack_ids[state->parent_depth],
                 sizeof(state->parent_stack_ids[0]), "%s", saved_id);
        snprintf(state->parent_stack_names[state->parent_depth],
                 sizeof(state->parent_stack_names[0]), "%s", saved_name);
        state->parent_depth++;
    }

    state->current_view = VIEW_BROWSE;
    state->selected_index = 0;
    state->scroll_offset = 0;

    /* Show loading indicator before blocking API call */
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(s_text_buf);
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);
    draw_text(115, 110, 0.6f, rgba(COLOR_PRIMARY), "Loading...");
    C3D_FrameEnd(0);

    /* Fetch into state->items directly (no stack-heavy temp copy) */
    jfin_get_items(session, saved_id, 0, JFIN_MAX_ITEMS, &state->items);

    if (state->items.count == 0) {
        /* Empty folder — undo navigation */
        state->parent_depth--;
        ui_navigate_back(state, session);
    }
}

void ui_navigate_back(ui_state_t *state, const jfin_session_t *session)
{
    if (state->parent_depth <= 0) {
        /* Already at top — go to libraries */
        state->current_view = VIEW_LIBRARIES;
        jfin_get_views(session, &state->items);
        state->selected_index = 0;
        state->scroll_offset = 0;
        return;
    }

    state->parent_depth--;
    state->selected_index = 0;
    state->scroll_offset = 0;

    if (state->parent_depth == 0) {
        state->current_view = VIEW_LIBRARIES;
        jfin_get_views(session, &state->items);
    } else {
        const char *parent_id = state->parent_stack_ids[state->parent_depth - 1];
        jfin_get_items(session, parent_id, 0, JFIN_MAX_ITEMS, &state->items);
    }
}

/* ── Renderers ─────────────────────────────────────────────────────── */

void ui_render_login(const ui_state_t *state)
{
    /* Bottom screen: login form */
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);

    draw_text(10, 10, 0.7f, rgba(COLOR_PRIMARY), "Connect to Jellyfin Server");

    const char *labels[] = {"Server URL:", "Username:", "Password:"};
    const char *values[] = {state->server_url, state->username, "********"};

    for (int i = 0; i < 3; i++) {
        float y = 50 + i * 50;
        u32 bg = (i == state->login_field) ? rgba(COLOR_HIGHLIGHT) : rgba(COLOR_BG_CARD);
        draw_rect(10, y, 300, 40, bg);
        draw_text(15, y + 2, 0.45f, rgba(COLOR_TEXT_SECONDARY), labels[i]);
        draw_text(15, y + 18, 0.5f, rgba(COLOR_TEXT_PRIMARY),
                  values[i][0] ? values[i] : "(tap A to enter)");
    }

    draw_text(10, 210, 0.45f, rgba(COLOR_TEXT_SECONDARY),
              "A: Edit field  R: Connect  START: Exit");
}

void ui_render_libraries(const ui_state_t *state)
{
    /* Bottom screen: library list */
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);

    draw_text(10, 5, 0.55f, rgba(COLOR_PRIMARY), "Libraries");

    for (int i = 0; i < state->items.count && i < UI_MAX_VISIBLE_ITEMS; i++) {
        int idx = state->scroll_offset + i;
        if (idx >= state->items.count) break;

        float y = 30 + i * UI_LIST_ITEM_HEIGHT;
        u32 bg = (idx == state->selected_index) ? rgba(COLOR_HIGHLIGHT) : rgba(COLOR_BG_CARD);
        draw_rect(5, y, 310, UI_LIST_ITEM_HEIGHT - 4, bg);

        draw_text(15, y + 10, 0.55f, rgba(COLOR_TEXT_PRIMARY),
                  state->items.items[idx].name);
    }

    draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
              "A:Enter B:Back Y:Playing SEL:Search");
}

void ui_render_browse(const ui_state_t *state)
{
    /* Bottom screen: item list */
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);

    /* Breadcrumb */
    if (state->parent_depth > 0) {
        draw_text(10, 5, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->parent_stack_names[state->parent_depth - 1]);
    }

    for (int i = 0; i < UI_MAX_VISIBLE_ITEMS; i++) {
        int idx = state->scroll_offset + i;
        if (idx >= state->items.count) break;

        float y = 25 + i * UI_LIST_ITEM_HEIGHT;
        const jfin_item_t *item = &state->items.items[idx];

        u32 bg = (idx == state->selected_index) ? rgba(COLOR_HIGHLIGHT) : rgba(COLOR_BG_CARD);
        draw_rect(5, y, 310, UI_LIST_ITEM_HEIGHT - 4, bg);

        /* Item name */
        char label[160];
        if (item->type == JFIN_ITEM_AUDIO && item->index_number > 0) {
            snprintf(label, sizeof(label), "%d. %s", item->index_number, item->name);
        } else if (item->type == JFIN_ITEM_EPISODE && item->index_number > 0) {
            snprintf(label, sizeof(label), "E%d - %s", item->index_number, item->name);
        } else {
            snprintf(label, sizeof(label), "%s", item->name);
        }

        draw_text(15, y + 4, 0.5f, rgba(COLOR_TEXT_PRIMARY), label);

        /* Subtitle: artist/year/duration */
        char sub[128] = {0};
        if (item->artist[0]) {
            snprintf(sub, sizeof(sub), "%s", item->artist);
        } else if (item->year > 0) {
            snprintf(sub, sizeof(sub), "%d", item->year);
        }
        if (item->runtime_ticks > 0) {
            char dur[16];
            format_ticks(item->runtime_ticks, dur, sizeof(dur));
            if (sub[0]) {
                strncat(sub, " - ", sizeof(sub) - strlen(sub) - 1);
                strncat(sub, dur, sizeof(sub) - strlen(sub) - 1);
            } else {
                snprintf(sub, sizeof(sub), "%s", dur);
            }
        }
        if (sub[0])
            draw_text(15, y + 22, 0.4f, rgba(COLOR_TEXT_SECONDARY), sub);
    }

    /* Scroll + page indicator */
    if (state->items.total_count > 0) {
        char info[48];
        if (state->items.total_count > JFIN_MAX_ITEMS) {
            int page = (state->items.start_index / JFIN_MAX_ITEMS) + 1;
            int total_pages = (state->items.total_count + JFIN_MAX_ITEMS - 1) / JFIN_MAX_ITEMS;
            snprintf(info, sizeof(info), "pg %d/%d (%d total)", page, total_pages, state->items.total_count);
        } else {
            snprintf(info, sizeof(info), "%d/%d",
                     state->selected_index + 1, state->items.count);
        }
        draw_text(220, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY), info);
    }

    if (state->items.total_count > JFIN_MAX_ITEMS)
        draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY), "A:Sel B:Back L/R:Pg SEL:Search");
    else
        draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY), "A:Sel B:Back Y:Playing SEL:Search");
}

void ui_render_now_playing(const ui_state_t *state, const player_status_t *player)
{
    video_status_t vstatus = video_player_get_status();
    bool is_video = (vstatus.state != VIDEO_STOPPED);

    /* Top screen */
    C2D_TargetClear(s_top, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_top);

    if (!state->has_now_playing) {
        draw_text(120, 110, 0.7f, rgba(COLOR_TEXT_SECONDARY), "Nothing playing");
        return;
    }

    if (is_video && vstatus.state == VIDEO_ERROR) {
        /* Show error prominently on top screen */
        draw_text(50, 60, 0.7f, rgba(0xFF4444FF), "Playback Error");
        draw_text(30, 100, 0.5f, rgba(COLOR_TEXT_PRIMARY),
                  vstatus.error_msg[0] ? vstatus.error_msg : "Cannot play this content");
        draw_text(30, 140, 0.5f, rgba(COLOR_TEXT_SECONDARY),
                  state->now_playing.name);
        draw_text(60, 190, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  "Press any button to go back");
    } else if (is_video && vstatus.state == VIDEO_LOADING) {
        /* Show buffering indicator while video is loading */
        draw_text(130, 100, 0.7f, rgba(COLOR_PRIMARY), "Buffering...");
        draw_text(80, 135, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->now_playing.name);
    } else if (is_video) {
        /* Render video frame on top screen */
        video_player_render_frame();
    } else if (player->state == PLAYER_LOADING) {
        /* Show buffering indicator while audio is loading */
        draw_text(130, 100, 0.7f, rgba(COLOR_PRIMARY), "Buffering...");
        draw_text(80, 135, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->now_playing.name);
    } else {
        /* Audio-only: show track info */
        const jfin_item_t *item = &state->now_playing;

        draw_rect(125, 20, 150, 150, rgba(COLOR_BG_CARD));
        draw_text(165, 85, 0.6f, rgba(COLOR_TEXT_SECONDARY), "ART");

        draw_text(50, 180, 0.6f, rgba(COLOR_TEXT_PRIMARY), item->name);

        if (item->artist[0])
            draw_text(50, 200, 0.45f, rgba(COLOR_ACCENT), item->artist);

        if (item->album[0])
            draw_text(50, 215, 0.4f, rgba(COLOR_TEXT_SECONDARY), item->album);
    }

    /* Bottom screen: transport controls */
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);

    /* Use video position/state if video is playing, otherwise audio */
    int64_t pos_ticks, dur_ticks;
    int buf_pct;
    const char *state_str = "STOPPED";

    if (is_video) {
        pos_ticks = vstatus.position_ticks;
        dur_ticks = vstatus.duration_ticks;
        buf_pct = vstatus.buffer_percent;
        switch (vstatus.state) {
        case VIDEO_LOADING:  state_str = "BUFFERING..."; break;
        case VIDEO_PLAYING:  state_str = "PLAYING"; break;
        case VIDEO_PAUSED:   state_str = "PAUSED"; break;
        case VIDEO_ERROR:    state_str = vstatus.error_msg; break;
        default: break;
        }
    } else {
        pos_ticks = player->position_ticks;
        dur_ticks = player->duration_ticks;
        buf_pct = player->buffer_percent;
        switch (player->state) {
        case PLAYER_LOADING:  state_str = "BUFFERING..."; break;
        case PLAYER_PLAYING:  state_str = "PLAYING"; break;
        case PLAYER_PAUSED:   state_str = "PAUSED"; break;
        case PLAYER_ERROR:    state_str = player->error_msg; break;
        default: break;
        }
    }

    /* Progress bar */
    float progress = 0.0f;
    if (dur_ticks > 0)
        progress = (float)pos_ticks / (float)dur_ticks;
    if (progress > 1.0f) progress = 1.0f;

    draw_rect(20, 40, 280, 6, rgba(COLOR_BG_CARD));
    draw_rect(20, 40, 280 * progress, 6, rgba(COLOR_PRIMARY));

    /* Time labels */
    char pos_str[16], dur_str[16];
    format_ticks(pos_ticks, pos_str, sizeof(pos_str));
    format_ticks(dur_ticks, dur_str, sizeof(dur_str));
    draw_text(20, 50, 0.4f, rgba(COLOR_TEXT_SECONDARY), pos_str);
    draw_text(270, 50, 0.4f, rgba(COLOR_TEXT_SECONDARY), dur_str);

    draw_text(110, 80, 0.55f, rgba(COLOR_PRIMARY), state_str);

    /* Buffer indicator */
    char buf_str[64];
    snprintf(buf_str, sizeof(buf_str), "Buffer: %d%%", buf_pct);
    draw_text(115, 100, 0.4f, rgba(COLOR_TEXT_SECONDARY), buf_str);

    /* Diagnostics for video playback */
    if (is_video) {
        char diag[64];
        snprintf(diag, sizeof(diag), "Dec: %.0f fps  Disp: %.0f fps  %dx%d",
                 vstatus.decode_fps, vstatus.display_fps,
                 vstatus.video_width, vstatus.video_height);
        draw_text(30, 125, 0.38f, rgba(COLOR_TEXT_SECONDARY), diag);
    }

    /* Controls hint */
    draw_text(60, 180, 0.5f, rgba(COLOR_TEXT_PRIMARY), "A: Pause   X: Stop   B: Back");
}

/* ── Main render dispatch ──────────────────────────────────────────── */

void ui_render(const ui_state_t *state, const jfin_session_t *session,
               const player_status_t *player)
{
    (void)session;

    C2D_TextBufClear(s_text_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    /* Top screen: branding or now-playing */
    if (state->current_view == VIEW_NOW_PLAYING) {
        ui_render_now_playing(state, player);
    } else {
        C2D_TargetClear(s_top, rgba(COLOR_BG_DARK));
        C2D_SceneBegin(s_top);

        draw_text(100, 80, 1.0f, rgba(COLOR_PRIMARY), "Jellyfin 3DS");
        draw_text(130, 120, 0.5f, rgba(COLOR_TEXT_SECONDARY), "v" JFIN_VERSION);

        /* Show mini now-playing bar if something is playing */
        if (state->has_now_playing && player->state == PLAYER_PLAYING) {
            draw_rect(0, 210, 400, 30, rgba(COLOR_BG_CARD));
            draw_text(10, 215, 0.45f, rgba(COLOR_TEXT_PRIMARY),
                      state->now_playing.name);
            draw_text(340, 215, 0.4f, rgba(COLOR_ACCENT), "Y: View");
        }
    }

    /* Bottom screen: view-specific */
    switch (state->current_view) {
    case VIEW_LOGIN:
        ui_render_login(state);
        break;
    case VIEW_LIBRARIES:
        ui_render_libraries(state);
        break;
    case VIEW_BROWSE:
        ui_render_browse(state);
        break;
    case VIEW_NOW_PLAYING:
        /* Already rendered above (both screens) */
        break;
    }

    C3D_FrameEnd(0);
}
