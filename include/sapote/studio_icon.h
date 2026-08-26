/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_STUDIO_ICON_H
#define SAPOTE_STUDIO_ICON_H

#include <stddef.h>
#include <stdint.h>

/* Uses the checked SRL decoder and status values declared by logo.h. */
int32_t sapote_studio_icon_geometry(uint32_t *width, uint32_t *height);
int32_t sapote_studio_icon_decode(
    uint32_t *out,
    size_t out_pixels,
    uint8_t red_shift,
    uint8_t green_shift,
    uint8_t blue_shift,
    uint32_t background
);
int32_t sapote_studio_icon_decode_alpha(uint8_t *out, size_t out_pixels);

#endif
