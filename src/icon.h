#pragma once
#include <stdint.h>
#include <d2d1.h>

#ifdef __cplusplus
extern "C" {
#endif

void icon_start(void);
void icon_stop(void);

/* Returns cached D2D bitmap for the given image_path intern handle.
   NULL if not loaded yet or extraction failed. Render-thread only.
   The returned pointer is owned by the cache — do NOT Release() it. */
ID2D1Bitmap *icon_query(uint32_t image_path_handle);

/* Called on D2DERR_RECREATE_TARGET — releases D2D bitmaps but keeps
   HICONs so they can be re-converted on next query. */
void icon_invalidate_bitmaps(void);

#ifdef __cplusplus
}
#endif
