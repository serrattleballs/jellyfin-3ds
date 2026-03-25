/**
 * album_art.h — Album art loading and display
 */

#ifndef JFIN_ALBUM_ART_H
#define JFIN_ALBUM_ART_H

#include <stdbool.h>
#include <citro2d.h>
#include "api/jellyfin.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load album art for an item. Downloads JPEG, decodes, uploads to texture.
 * Caches: only re-downloads if item_id changes.
 * Returns true if art is ready to display.
 */
bool album_art_load(const jfin_session_t *session, const jfin_item_t *item);

/**
 * Draw the loaded album art centered at (x, y) with given size.
 */
void album_art_draw(float x, float y, float size);

/**
 * Returns true if album art is currently loaded.
 */
bool album_art_is_loaded(void);

/**
 * Free the album art texture.
 */
void album_art_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
