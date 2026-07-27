#pragma once
#include <lvgl.h>

// The overview: column 0 of the usage tileview. One row per enabled+configured
// provider, up to 8 per page, paginated vertically past that. Row density is
// chosen from the height each row gets, so the zone is full at any count.

// Build the overview page tiles and their rows for the current provider set.
// `tiles_view` is the usage lv_tileview; `dot_parent` hosts the page-indicator
// dots (they float above the tileview, so they are not tile children).
// Call after lv_obj_clean(tiles_view) — it adds tiles at column 0.
void overview_build(lv_obj_t* tiles_view, lv_obj_t* dot_parent);

// Re-render every row from a fresh provider snapshot.
void overview_update(void);

// Number of vertical pages the overview occupies (>= 1).
int  overview_page_count(void);

// Tell the overview which tile the tileview settled on, so the vertical dot
// column can show the active page and hide itself off column 0.
void overview_set_active(int col, int page);
