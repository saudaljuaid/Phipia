/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/runtime.h>
#include <sapote/window.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CANVAS_WIDTH UINT32_C(420)
#define CANVAS_HEIGHT UINT32_C(250)
#define FRAME_NS UINT64_C(75000000)
#define RUN_NS UINT64_C(6000000000)
#define PULSE_X UINT32_C(346)
#define PULSE_Y UINT32_C(236)
#define PULSE_WIDTH UINT32_C(70)
#define PULSE_HEIGHT UINT32_C(14)
#define ARTBOARD_X UINT32_C(52)
#define ARTBOARD_Y UINT32_C(42)
#define ARTBOARD_WIDTH UINT32_C(296)
#define ARTBOARD_HEIGHT UINT32_C(190)
#define PALETTE_X INT32_C(384)
#define PALETTE_RADIUS INT32_C(9)
#define BRUSH_RADIUS INT32_C(3)

#define COLOR_BACKGROUND UINT32_C(0xC6CBD0)
#define COLOR_HEADER UINT32_C(0xEBEDEF)
#define COLOR_TOOLBAR UINT32_C(0xF4F5F6)
#define COLOR_WORKSPACE UINT32_C(0xD6D9DC)
#define COLOR_PAPER UINT32_C(0xFFFFFF)
#define COLOR_INK UINT32_C(0x202326)
#define COLOR_MUTED UINT32_C(0x687078)
#define COLOR_TEAL UINT32_C(0x26BFA4)
#define COLOR_CORAL UINT32_C(0xE95A55)
#define COLOR_VIOLET UINT32_C(0x7659D7)
#define COLOR_GOLD UINT32_C(0xF2B84B)
#define COLOR_BLUE UINT32_C(0x3D7DE0)

struct canvas {
    uint32_t *pixels;
    uint32_t stride;
};

static const uint32_t palette_colors[] = {
    COLOR_INK, COLOR_BLUE, COLOR_TEAL, COLOR_CORAL, COLOR_VIOLET, COLOR_GOLD
};

static const uint8_t glyphs[43][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}, /* 5 */
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}, /* 9 */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x04,0x00,0x00,0x04,0x00,0x00}, /* : */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04}, /* , */
    {0x00,0x00,0x00,0x00,0x00,0x06,0x04}  /* ! */
};

static const uint8_t *glyph(char character)
{
    if (character >= '0' && character <= '9') {
        return glyphs[(size_t)(character - '0')];
    }
    if (character >= 'A' && character <= 'Z') {
        return glyphs[10U + (size_t)(character - 'A')];
    }
    switch (character) {
    case ' ': return glyphs[36];
    case ':': return glyphs[37];
    case '-': return glyphs[38];
    case '.': return glyphs[39];
    case ',': return glyphs[40];
    case '!': return glyphs[41];
    default: return glyphs[42];
    }
}

static void fill_rect(struct canvas *canvas, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t color)
{
    for (uint32_t row = y; row < y + height && row < CANVAS_HEIGHT; ++row) {
        for (uint32_t column = x;
             column < x + width && column < CANVAS_WIDTH; ++column) {
            canvas->pixels[(size_t)row * canvas->stride + column] = color;
        }
    }
}

static void put_pixel(struct canvas *canvas, int32_t x, int32_t y,
    uint32_t color)
{
    if (x >= 0 && y >= 0 && (uint32_t)x < CANVAS_WIDTH &&
        (uint32_t)y < CANVAS_HEIGHT) {
        canvas->pixels[(size_t)(uint32_t)y * canvas->stride +
            (uint32_t)x] = color;
    }
}

static void stroke_rect(struct canvas *canvas, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t thickness, uint32_t color)
{
    if (width == 0U || height == 0U || thickness == 0U) {
        return;
    }
    fill_rect(canvas, x, y, width, thickness, color);
    fill_rect(canvas, x, y + height - thickness, width, thickness, color);
    fill_rect(canvas, x, y, thickness, height, color);
    fill_rect(canvas, x + width - thickness, y, thickness, height, color);
}

static void fill_circle(struct canvas *canvas, int32_t center_x,
    int32_t center_y, int32_t radius, uint32_t color)
{
    const int32_t squared = radius * radius;

    for (int32_t y = -radius; y <= radius; ++y) {
        for (int32_t x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= squared) {
                put_pixel(canvas, center_x + x, center_y + y, color);
            }
        }
    }
}

static void draw_ring(struct canvas *canvas, int32_t center_x,
    int32_t center_y, int32_t radius, int32_t thickness, uint32_t color)
{
    const int32_t outer = radius * radius;
    const int32_t inner_radius = radius - thickness;
    const int32_t inner = inner_radius * inner_radius;

    for (int32_t y = -radius; y <= radius; ++y) {
        for (int32_t x = -radius; x <= radius; ++x) {
            const int32_t distance = x * x + y * y;

            if (distance <= outer && distance >= inner) {
                put_pixel(canvas, center_x + x, center_y + y, color);
            }
        }
    }
}

static void draw_line(struct canvas *canvas, int32_t x0, int32_t y0,
    int32_t x1, int32_t y1, int32_t thickness, uint32_t color)
{
    const int32_t delta_x = x0 < x1 ? x1 - x0 : x0 - x1;
    const int32_t step_x = x0 < x1 ? 1 : -1;
    const int32_t delta_y = -(y0 < y1 ? y1 - y0 : y0 - y1);
    const int32_t step_y = y0 < y1 ? 1 : -1;
    int32_t error = delta_x + delta_y;

    for (;;) {
        const int32_t radius = thickness / 2;

        for (int32_t y = -radius; y <= radius; ++y) {
            for (int32_t x = -radius; x <= radius; ++x) {
                put_pixel(canvas, x0 + x, y0 + y, color);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int32_t doubled = error * 2;

        if (doubled >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (doubled <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

static void draw_text(struct canvas *canvas, uint32_t x, uint32_t y,
    const char *text, uint32_t color, uint32_t scale)
{
    while (*text != '\0') {
        const uint8_t *shape = glyph(*text++);

        for (uint32_t row = 0U; row < 7U; ++row) {
            for (uint32_t column = 0U; column < 5U; ++column) {
                if ((shape[row] & (uint8_t)(UINT8_C(1) << (4U - column))) !=
                        0U) {
                    fill_rect(canvas, x + column * scale, y + row * scale,
                        scale, scale, color);
                }
            }
        }
        x += 6U * scale;
    }
}

static void draw_palette(struct canvas *canvas, size_t selected)
{
    fill_rect(canvas, 356U, 32U, 64U, 204U, COLOR_TOOLBAR);
    fill_rect(canvas, 356U, 32U, 1U, 204U, UINT32_C(0xA8ADB2));
    draw_text(canvas, 369U, 42U, "COLOR", COLOR_MUTED, 1U);
    for (size_t index = 0U;
         index < sizeof(palette_colors) / sizeof(palette_colors[0]);
         ++index) {
        const int32_t y = 68 + (int32_t)index * 27;

        if (index == selected) {
            draw_ring(canvas, PALETTE_X, y, PALETTE_RADIUS + 4, 2,
                COLOR_INK);
        }
        fill_circle(canvas, PALETTE_X, y, PALETTE_RADIUS,
            palette_colors[index]);
        fill_circle(canvas, PALETTE_X - 3, y - 3, 2, UINT32_C(0xFFFFFF));
    }
}

static void draw_workspace(struct canvas *canvas, size_t selected,
    int focused)
{
    const uint32_t accent = palette_colors[selected];

    fill_rect(canvas, 0U, 0U, CANVAS_WIDTH, CANVAS_HEIGHT,
        COLOR_BACKGROUND);
    fill_rect(canvas, 0U, 0U, CANVAS_WIDTH, 32U, COLOR_HEADER);
    fill_rect(canvas, 0U, 31U, CANVAS_WIDTH, 1U, UINT32_C(0x969DA3));
    draw_text(canvas, 14U, 12U, "CANVAS", COLOR_INK, 2U);
    draw_text(canvas, 182U, 13U, "UNTITLED", COLOR_MUTED, 1U);
    draw_text(canvas, 350U, 13U, "100 PCT", COLOR_MUTED, 1U);

    fill_rect(canvas, 0U, 32U, 44U, 204U, COLOR_TOOLBAR);
    fill_rect(canvas, 43U, 32U, 1U, 204U, UINT32_C(0xA8ADB2));
    fill_rect(canvas, 44U, 32U, 312U, 204U, COLOR_WORKSPACE);
    fill_rect(canvas, ARTBOARD_X + 3U, ARTBOARD_Y + 4U,
        ARTBOARD_WIDTH, ARTBOARD_HEIGHT, UINT32_C(0x9CA2A8));
    fill_rect(canvas, ARTBOARD_X, ARTBOARD_Y, ARTBOARD_WIDTH,
        ARTBOARD_HEIGHT, COLOR_PAPER);

    fill_rect(canvas, 6U, 42U, 32U, 32U, UINT32_C(0xDDE1E4));
    fill_rect(canvas, 6U, 42U, 3U, 32U, accent);
    draw_line(canvas, 14, 65, 31, 48, 3, COLOR_INK);
    fill_circle(canvas, 31, 48, 3, accent);
    draw_ring(canvas, 22, 94, 8, 2, COLOR_MUTED);
    stroke_rect(canvas, 14U, 116U, 17U, 14U, 2U, COLOR_MUTED);
    draw_line(canvas, 13, 158, 22, 143, 2, COLOR_MUTED);
    draw_line(canvas, 22, 143, 32, 160, 2, COLOR_MUTED);
    fill_circle(canvas, 13, 158, 2, COLOR_MUTED);
    fill_circle(canvas, 22, 143, 2, COLOR_MUTED);
    fill_circle(canvas, 32, 160, 2, COLOR_MUTED);
    draw_line(canvas, 15, 188, 30, 188, 2, COLOR_MUTED);
    draw_line(canvas, 22, 181, 22, 196, 2, COLOR_MUTED);

    draw_palette(canvas, selected);
    fill_rect(canvas, 0U, 236U, CANVAS_WIDTH, 14U, COLOR_HEADER);
    draw_text(canvas, 12U, 238U, "BRUSH 7", COLOR_MUTED, 1U);
    draw_text(canvas, 160U, 238U, "DRAG TO DRAW", COLOR_MUTED, 1U);
    fill_circle(canvas, 365, 242, 3, accent);
    draw_text(canvas, 375U, 239U, "LIVE", COLOR_INK, 1U);
    stroke_rect(canvas, ARTBOARD_X - 1U, ARTBOARD_Y - 1U,
        ARTBOARD_WIDTH + 2U, ARTBOARD_HEIGHT + 2U, 1U,
        focused != 0 ? accent : UINT32_C(0x8C9298));
}

static bool point_in_artboard(int32_t x, int32_t y)
{
    return x >= (int32_t)ARTBOARD_X && y >= (int32_t)ARTBOARD_Y &&
        x < (int32_t)(ARTBOARD_X + ARTBOARD_WIDTH) &&
        y < (int32_t)(ARTBOARD_Y + ARTBOARD_HEIGHT);
}

static size_t palette_at(int32_t x, int32_t y)
{
    for (size_t index = 0U;
         index < sizeof(palette_colors) / sizeof(palette_colors[0]);
         ++index) {
        const int32_t center_y = 68 + (int32_t)index * 27;
        const int32_t delta_x = x - PALETTE_X;
        const int32_t delta_y = y - center_y;
        const int32_t radius = PALETTE_RADIUS + 5;

        if (delta_x * delta_x + delta_y * delta_y <= radius * radius) {
            return index;
        }
    }
    return SIZE_MAX;
}

static int32_t clamp_artboard_x(int32_t x)
{
    if (x < (int32_t)ARTBOARD_X) {
        return (int32_t)ARTBOARD_X;
    }
    if (x >= (int32_t)(ARTBOARD_X + ARTBOARD_WIDTH)) {
        return (int32_t)(ARTBOARD_X + ARTBOARD_WIDTH - 1U);
    }
    return x;
}

static int32_t clamp_artboard_y(int32_t y)
{
    if (y < (int32_t)ARTBOARD_Y) {
        return (int32_t)ARTBOARD_Y;
    }
    if (y >= (int32_t)(ARTBOARD_Y + ARTBOARD_HEIGHT)) {
        return (int32_t)(ARTBOARD_Y + ARTBOARD_HEIGHT - 1U);
    }
    return y;
}

static struct sapote_rect brush_damage(int32_t from_x, int32_t from_y,
    int32_t to_x, int32_t to_y)
{
    int32_t left = from_x < to_x ? from_x : to_x;
    int32_t top = from_y < to_y ? from_y : to_y;
    int32_t right = from_x > to_x ? from_x : to_x;
    int32_t bottom = from_y > to_y ? from_y : to_y;

    left = clamp_artboard_x(left - BRUSH_RADIUS);
    top = clamp_artboard_y(top - BRUSH_RADIUS);
    right = clamp_artboard_x(right + BRUSH_RADIUS);
    bottom = clamp_artboard_y(bottom + BRUSH_RADIUS);
    return (struct sapote_rect){
        (uint32_t)left, (uint32_t)top,
        (uint32_t)(right - left + 1), (uint32_t)(bottom - top + 1)
    };
}

static int present(sapote_handle_t window, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
    const struct sapote_rect damage = {x, y, width, height};
    return sapote_surface_present(window, &damage, 1U) < 0 ? -1 : 0;
}

int main(int argc, char **argv, char **environment)
{
    struct sapote_window_create_response response;
    struct canvas canvas;
    uint64_t started;
    uint64_t next_pulse;
    uint32_t pulse = 0U;
    uint32_t partial_presents = 0U;
    uint32_t focus_events = 0U;
    uint32_t key_events = 0U;
    uint32_t pointer_events = 0U;
    uint32_t present_samples = 0U;
    uint32_t stroke_segments = 0U;
    uint32_t color_changes = 0U;
    size_t selected = 2U;
    uint64_t present_elapsed = 0U;
    int32_t previous_x = 0;
    int32_t previous_y = 0;
    int focused = 0;
    int drawing = 0;
    int proof_mode = argc > 1 && strcmp(argv[1], "multi-window-proof") == 0;
    int running = 1;

    (void)environment;
    if (sapote_window_create("Canvas", CANVAS_WIDTH,
            CANVAS_HEIGHT, &response) != 0 ||
        response.surface_address == 0U ||
        response.stride_bytes != CANVAS_WIDTH * sizeof(uint32_t)) {
        return 20;
    }
    canvas.pixels = (uint32_t *)(uintptr_t)response.surface_address;
    canvas.stride = response.stride_bytes / sizeof(uint32_t);
    draw_workspace(&canvas, selected, focused);
    if (present(response.window, 0U, 0U, CANVAS_WIDTH, CANVAS_HEIGHT) != 0) {
        return 21;
    }
    printf("SAPOTE CANVAS READY width=%u height=%u\n", response.width,
        response.height);
    started = sapote_monotonic_ns();
    next_pulse = started;
    while (running != 0 && (proof_mode == 0 ||
            sapote_monotonic_ns() - started < RUN_NS)) {
        const uint64_t now = sapote_monotonic_ns();
        struct sapote_event event;
        long wait_result;

        if (now >= next_pulse) {
            const uint32_t color = (pulse & 1U) == 0U ?
                palette_colors[selected] : COLOR_GOLD;
            uint64_t present_started;

            fill_rect(&canvas, PULSE_X, PULSE_Y, PULSE_WIDTH, PULSE_HEIGHT,
                COLOR_HEADER);
            fill_circle(&canvas, 365, 242, 3, color);
            draw_text(&canvas, 375U, 239U, "LIVE", COLOR_INK, 1U);
            present_started = sapote_monotonic_ns();
            if (present(response.window, PULSE_X, PULSE_Y, PULSE_WIDTH,
                    PULSE_HEIGHT) != 0) {
                return 22;
            }
            present_elapsed += sapote_monotonic_ns() - present_started;
            ++present_samples;
            ++partial_presents;
            ++pulse;
            next_pulse = now + UINT64_C(250000000);
        }
        wait_result = sapote_event_wait(response.events, now + FRAME_NS);
        if (wait_result < 0 && wait_result != -SAPOTE_ETIMEDOUT) {
            return 23;
        }
        while (sapote_event_read(response.events, &event) > 0) {
            if (event.type == SAPOTE_EVENT_FOCUS) {
                ++focus_events;
                focused = event.value != 0U;
                stroke_rect(&canvas, ARTBOARD_X - 1U, ARTBOARD_Y - 1U,
                    ARTBOARD_WIDTH + 2U, ARTBOARD_HEIGHT + 2U, 1U,
                    focused != 0 ? palette_colors[selected] :
                        UINT32_C(0x8C9298));
                if (present(response.window, ARTBOARD_X - 1U,
                        ARTBOARD_Y - 1U, ARTBOARD_WIDTH + 2U,
                        ARTBOARD_HEIGHT + 2U) != 0) {
                    return 24;
                }
                ++partial_presents;
            } else if (event.type == SAPOTE_EVENT_KEY &&
                event.value != SAPOTE_KEY_RELEASED) {
                ++key_events;
                selected = (selected + 1U) %
                    (sizeof(palette_colors) / sizeof(palette_colors[0]));
                ++color_changes;
                draw_palette(&canvas, selected);
                if (present(response.window, 356U, 32U, 64U, 204U) != 0) {
                    return 25;
                }
                ++partial_presents;
            } else if (event.type == SAPOTE_EVENT_POINTER_MOVE) {
                ++pointer_events;
                if (drawing != 0) {
                    const int32_t x = clamp_artboard_x(event.x);
                    const int32_t y = clamp_artboard_y(event.y);
                    const struct sapote_rect damage = brush_damage(previous_x,
                        previous_y, x, y);

                    draw_line(&canvas, previous_x, previous_y, x, y,
                        BRUSH_RADIUS * 2 + 1, palette_colors[selected]);
                    previous_x = x;
                    previous_y = y;
                    ++stroke_segments;
                    if (present(response.window, damage.x, damage.y,
                            damage.width, damage.height) != 0) {
                        return 26;
                    }
                    ++partial_presents;
                }
            } else if (event.type == SAPOTE_EVENT_POINTER_BUTTON) {
                ++pointer_events;
                if (event.value != 0U) {
                    const size_t palette = palette_at(event.x, event.y);

                    if (palette != SIZE_MAX) {
                        selected = palette;
                        ++color_changes;
                        draw_palette(&canvas, selected);
                        if (present(response.window, 356U, 32U, 64U,
                                204U) != 0) {
                            return 26;
                        }
                        ++partial_presents;
                    } else if (point_in_artboard(event.x, event.y)) {
                        const struct sapote_rect damage = brush_damage(event.x,
                            event.y, event.x, event.y);

                        drawing = 1;
                        previous_x = event.x;
                        previous_y = event.y;
                        fill_circle(&canvas, event.x, event.y, BRUSH_RADIUS,
                            palette_colors[selected]);
                        (void)sapote_pointer_capture(response.window, 1);
                        ++stroke_segments;
                        if (present(response.window, damage.x, damage.y,
                                damage.width, damage.height) != 0) {
                            return 26;
                        }
                        ++partial_presents;
                    }
                } else if (drawing != 0) {
                    drawing = 0;
                    (void)sapote_pointer_capture(response.window, 0);
                }
            } else if (event.type == SAPOTE_EVENT_CLOSE) {
                running = 0;
            }
        }
    }
    printf("SAPOTE CANVAS PASS focus=%u key=%u pointer=%u strokes=%u colors=%u partial=%u\n",
        focus_events, key_events, pointer_events, stroke_segments,
        color_changes, partial_presents);
    printf("SAPOTE PERF canvas damage=70x14 samples=%u total_ns=%llu average_ns=%llu\n",
        present_samples, (unsigned long long)present_elapsed,
        (unsigned long long)(present_samples == 0U ? 0U :
            present_elapsed / present_samples));
    if (sapote_handle_close(response.events) < 0 ||
        sapote_handle_close(response.window) < 0 || partial_presents < 10U ||
        focus_events == 0U || (proof_mode != 0 && key_events != 0U &&
            color_changes == 0U)) {
        return 27;
    }
    return 0;
}
