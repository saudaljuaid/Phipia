/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/runtime.h>
#include <sapote/window.h>

#include <stdint.h>
#include <stdio.h>

#define CANVAS_WIDTH UINT32_C(420)
#define CANVAS_HEIGHT UINT32_C(250)
#define FRAME_NS UINT64_C(75000000)
#define RUN_NS UINT64_C(5000000000)
#define PULSE_X UINT32_C(338)
#define PULSE_Y UINT32_C(232)
#define PULSE_WIDTH UINT32_C(70)
#define PULSE_HEIGHT UINT32_C(14)

#define COLOR_BACKGROUND UINT32_C(0x101620)
#define COLOR_HEADER UINT32_C(0x151C27)
#define COLOR_TOOLBAR UINT32_C(0x121923)
#define COLOR_WORKSPACE UINT32_C(0x202833)
#define COLOR_PAPER UINT32_C(0xEEE9DF)
#define COLOR_INK UINT32_C(0x17202A)
#define COLOR_MUTED UINT32_C(0x7D8B9C)
#define COLOR_TEAL UINT32_C(0x27C7A5)
#define COLOR_CORAL UINT32_C(0xF06A5D)
#define COLOR_VIOLET UINT32_C(0x7367F0)
#define COLOR_GOLD UINT32_C(0xE9B44C)

struct canvas {
    uint32_t *pixels;
    uint32_t stride;
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

static void draw_artboard(struct canvas *canvas, uint32_t accent)
{
    static const int32_t path[][2] = {
        {96, 164}, {128, 126}, {159, 151}, {192, 112},
        {224, 138}, {253, 116}, {318, 168}
    };

    fill_rect(canvas, 79U, 61U, 274U, 158U, UINT32_C(0x0B1017));
    fill_rect(canvas, 74U, 56U, 274U, 158U, COLOR_PAPER);
    fill_circle(canvas, 284, 101, 43, COLOR_CORAL);
    fill_circle(canvas, 303, 118, 20, COLOR_PAPER);
    fill_circle(canvas, 112, 173, 19, COLOR_GOLD);
    for (size_t index = 1U; index < sizeof(path) / sizeof(path[0]); ++index) {
        draw_line(canvas, path[index - 1U][0], path[index - 1U][1],
            path[index][0], path[index][1], 5, COLOR_INK);
        draw_line(canvas, path[index - 1U][0], path[index - 1U][1],
            path[index][0], path[index][1], 2, accent);
    }
    fill_circle(canvas, 96, 164, 5, accent);
    fill_circle(canvas, 318, 168, 5, accent);
    draw_text(canvas, 96U, 75U, "SAPOTE", COLOR_INK, 2U);
    draw_text(canvas, 98U, 100U, "NATIVE CANVAS", UINT32_C(0x556273), 1U);
    draw_text(canvas, 291U, 188U, "01", COLOR_INK, 2U);
}

static void draw_workspace(struct canvas *canvas, uint32_t accent,
    int focused)
{
    fill_rect(canvas, 0U, 0U, CANVAS_WIDTH, CANVAS_HEIGHT,
        COLOR_BACKGROUND);
    fill_rect(canvas, 0U, 0U, CANVAS_WIDTH, 44U, COLOR_HEADER);
    fill_rect(canvas, 0U, 44U, 48U, 184U, COLOR_TOOLBAR);
    fill_rect(canvas, 48U, 44U, CANVAS_WIDTH - 48U, 184U,
        COLOR_WORKSPACE);
    fill_rect(canvas, 0U, 228U, CANVAS_WIDTH, 22U, COLOR_BACKGROUND);
    fill_rect(canvas, 18U, 14U, 16U, 16U, accent);
    draw_line(canvas, 22, 26, 30, 18, 2, COLOR_PAPER);
    draw_text(canvas, 46U, 14U, "CANVAS", COLOR_PAPER, 2U);
    draw_text(canvas, 330U, 18U, "NATIVE ABI 1", COLOR_MUTED, 1U);

    fill_rect(canvas, 8U, 57U, 32U, 32U, UINT32_C(0x273241));
    fill_rect(canvas, 8U, 57U, 2U, 32U, accent);
    draw_line(canvas, 17, 79, 32, 64, 2, COLOR_PAPER);
    draw_ring(canvas, 24, 112, 8, 2, COLOR_MUTED);
    stroke_rect(canvas, 17U, 137U, 15U, 13U, 2U, COLOR_MUTED);
    draw_line(canvas, 16, 179, 23, 168, 2, COLOR_MUTED);
    draw_line(canvas, 23, 168, 32, 181, 2, COLOR_MUTED);
    fill_circle(canvas, 16, 179, 2, COLOR_MUTED);
    fill_circle(canvas, 23, 168, 2, COLOR_MUTED);
    fill_circle(canvas, 32, 181, 2, COLOR_MUTED);

    draw_artboard(canvas, accent);
    fill_rect(canvas, 360U, 56U, 1U, 158U, UINT32_C(0x35404D));
    fill_circle(canvas, 386, 75, 9, COLOR_TEAL);
    fill_circle(canvas, 386, 105, 9, COLOR_CORAL);
    fill_circle(canvas, 386, 135, 9, COLOR_VIOLET);
    fill_circle(canvas, 386, 165, 9, COLOR_GOLD);
    draw_ring(canvas, 386, accent == COLOR_VIOLET ? 135 : 75, 13, 1,
        COLOR_PAPER);
    draw_text(canvas, 376U, 195U, "01", COLOR_MUTED, 2U);

    draw_text(canvas, 14U, 236U, "RECT 70 X 14", COLOR_MUTED, 1U);
    fill_rect(canvas, PULSE_X, PULSE_Y, PULSE_WIDTH, PULSE_HEIGHT,
        COLOR_BACKGROUND);
    fill_circle(canvas, 365, 239, 3, accent);
    draw_text(canvas, 375U, 236U, "LIVE", COLOR_PAPER, 1U);
    stroke_rect(canvas, 70U, 52U, 282U, 166U, 1U,
        focused != 0 ? accent : UINT32_C(0x4B5664));
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
    uint32_t accent = COLOR_TEAL;
    uint64_t present_elapsed = 0U;
    int focused = 0;
    int running = 1;

    (void)argc;
    (void)argv;
    (void)environment;
    if (sapote_window_create("Canvas", CANVAS_WIDTH,
            CANVAS_HEIGHT, &response) != 0 ||
        response.surface_address == 0U ||
        response.stride_bytes != CANVAS_WIDTH * sizeof(uint32_t)) {
        return 20;
    }
    canvas.pixels = (uint32_t *)(uintptr_t)response.surface_address;
    canvas.stride = response.stride_bytes / sizeof(uint32_t);
    draw_workspace(&canvas, accent, focused);
    if (present(response.window, 0U, 0U, CANVAS_WIDTH, CANVAS_HEIGHT) != 0) {
        return 21;
    }
    printf("SAPOTE CANVAS READY width=%u height=%u\n", response.width,
        response.height);
    started = sapote_monotonic_ns();
    next_pulse = started;
    while (running != 0 && sapote_monotonic_ns() - started < RUN_NS) {
        const uint64_t now = sapote_monotonic_ns();
        struct sapote_event event;
        long wait_result;

        if (now >= next_pulse) {
            const uint32_t color = (pulse & 1U) == 0U ?
                accent : COLOR_GOLD;
            uint64_t present_started;

            fill_rect(&canvas, PULSE_X, PULSE_Y, PULSE_WIDTH, PULSE_HEIGHT,
                COLOR_BACKGROUND);
            fill_circle(&canvas, 365, 239, 3, color);
            draw_text(&canvas, 375U, 236U, "LIVE", COLOR_PAPER, 1U);
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
                stroke_rect(&canvas, 70U, 52U, 282U, 166U, 1U,
                    focused != 0 ? accent : UINT32_C(0x4B5664));
                if (present(response.window, 70U, 52U, 282U, 166U) != 0) {
                    return 24;
                }
                ++partial_presents;
            } else if (event.type == SAPOTE_EVENT_KEY &&
                event.value != SAPOTE_KEY_RELEASED) {
                ++key_events;
                accent = (key_events & 1U) == 0U ? COLOR_TEAL : COLOR_VIOLET;
                draw_workspace(&canvas, accent, focused);
                (void)sapote_pointer_capture(response.window, 1);
                if (present(response.window, 0U, 0U, CANVAS_WIDTH,
                        CANVAS_HEIGHT) != 0) {
                    return 25;
                }
                ++partial_presents;
            } else if (event.type == SAPOTE_EVENT_POINTER_MOVE ||
                event.type == SAPOTE_EVENT_POINTER_BUTTON) {
                ++pointer_events;
                fill_rect(&canvas, PULSE_X, PULSE_Y, PULSE_WIDTH,
                    PULSE_HEIGHT, COLOR_BACKGROUND);
                fill_circle(&canvas, 365, 239, 3,
                    (pointer_events & 1U) == 0U ? accent : COLOR_CORAL);
                draw_text(&canvas, 375U, 236U, "LIVE", COLOR_PAPER, 1U);
                if (event.type == SAPOTE_EVENT_POINTER_BUTTON) {
                    (void)sapote_pointer_capture(response.window,
                        event.value != 0U);
                }
                if (present(response.window, PULSE_X, PULSE_Y, PULSE_WIDTH,
                        PULSE_HEIGHT) != 0) {
                    return 26;
                }
                ++partial_presents;
            } else if (event.type == SAPOTE_EVENT_CLOSE) {
                running = 0;
            }
        }
    }
    printf("SAPOTE CANVAS PASS focus=%u key=%u pointer=%u partial=%u\n",
        focus_events, key_events, pointer_events, partial_presents);
    printf("SAPOTE PERF canvas damage=70x14 samples=%u total_ns=%llu average_ns=%llu\n",
        present_samples, (unsigned long long)present_elapsed,
        (unsigned long long)(present_samples == 0U ? 0U :
            present_elapsed / present_samples));
    if (sapote_handle_close(response.events) < 0 ||
        sapote_handle_close(response.window) < 0 || partial_presents < 10U ||
        focus_events == 0U) {
        return 27;
    }
    return 0;
}
