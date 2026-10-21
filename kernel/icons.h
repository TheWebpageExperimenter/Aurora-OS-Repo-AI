#pragma once
#include "kernel.h"
enum {
    ICON_LOGO, ICON_SEARCH, ICON_FILES, ICON_NOTES, ICON_MONITOR, ICON_SETTINGS,
    ICON_POWER, ICON_DOC, ICON_CLOSE, ICON_MIN, ICON_CHEV_L, ICON_CHEV_R, ICON_CHEV_UP,
    ICON_CHECK
};
/* Draws icon `id` into the box (x, y, size, size). `col` tints the monochrome glyph icons. */
void icon_draw(int id, int x, int y, int size, uint32_t col);
