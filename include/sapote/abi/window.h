/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_ABI_WINDOW_H
#define SAPOTE_ABI_WINDOW_H

#include <sapote/abi/base.h>

#define SAPOTE_WINDOW_TITLE_MAX 31U
#define SAPOTE_DAMAGE_MAX 8U
#define SAPOTE_PIXEL_XRGB8888 UINT32_C(1)

struct sapote_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct sapote_window_create_request {
    uint32_t size;
    uint32_t version;
    uint64_t title;
    uint32_t title_length;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct sapote_window_create_response {
    uint32_t size;
    uint32_t version;
    sapote_handle_t window;
    sapote_handle_t events;
    uint64_t surface_address;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t pixel_format;
} __attribute__((packed));

struct sapote_present_request {
    uint32_t size;
    uint32_t version;
    sapote_handle_t window;
    uint64_t rectangles;
    uint32_t rectangle_count;
    uint32_t flags;
} __attribute__((packed));

enum sapote_event_type {
    SAPOTE_EVENT_NONE = 0,
    SAPOTE_EVENT_KEY = 1,
    SAPOTE_EVENT_POINTER_MOVE = 2,
    SAPOTE_EVENT_POINTER_BUTTON = 3,
    SAPOTE_EVENT_FOCUS = 4,
    SAPOTE_EVENT_CLOSE = 5,
    SAPOTE_EVENT_QUEUE_OVERFLOW = 6
};

enum sapote_key_action {
    SAPOTE_KEY_RELEASED = 0,
    SAPOTE_KEY_PRESSED = 1,
    SAPOTE_KEY_REPEATED = 2
};

struct sapote_event {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    uint32_t flags;
    uint64_t monotonic_ns;
    int32_t x;
    int32_t y;
    int32_t delta_x;
    int32_t delta_y;
    uint32_t code;
    uint32_t value;
    uint32_t modifiers;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct sapote_rect) == 16U,
    "Sapote rectangle ABI changed");
_Static_assert(sizeof(struct sapote_window_create_request) == 40U,
    "Sapote window-create request ABI changed");
_Static_assert(sizeof(struct sapote_window_create_response) == 48U,
    "Sapote window-create response ABI changed");
_Static_assert(sizeof(struct sapote_present_request) == 32U,
    "Sapote present ABI changed");
_Static_assert(sizeof(struct sapote_event) == 56U,
    "Sapote event ABI changed");

#endif
