/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_WINDOW_H
#define SAPOTE_WINDOW_H

#include <stddef.h>
#include <stdint.h>
#include <sapote/abi.h>

int sapote_window_create(const char *title, uint32_t width, uint32_t height,
    struct sapote_window_create_response *response);
long sapote_surface_present(sapote_handle_t window,
    const struct sapote_rect *rectangles, size_t count);
long sapote_event_read(sapote_handle_t events, struct sapote_event *event);
long sapote_event_wait(sapote_handle_t events, uint64_t deadline_ns);
long sapote_pointer_capture(sapote_handle_t window, int capture);

#endif
