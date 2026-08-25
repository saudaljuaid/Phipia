/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_WALLPAPER_H
#define SAPOTE_WALLPAPER_H

#include <stddef.h>
#include <stdint.h>

enum wallpaper_status {
    WALLPAPER_STATUS_OK = 0,
    WALLPAPER_STATUS_NULL_ARGUMENT = 1,
    WALLPAPER_STATUS_BAD_HEADER = 2,
    WALLPAPER_STATUS_BAD_GEOMETRY = 3,
    WALLPAPER_STATUS_BAD_PALETTE = 4,
    WALLPAPER_STATUS_BUFFER_TOO_SMALL = 5
};

int32_t sapote_wallpaper_self_test(void);
size_t sapote_wallpaper_size(void);
int32_t sapote_wallpaper_geometry(uint32_t *width, uint32_t *height);
int32_t sapote_wallpaper_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift
);

#endif
