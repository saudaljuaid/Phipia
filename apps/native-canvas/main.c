/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/runtime.h>
#include <sapote/window.h>

#include <stdint.h>
#include <stdio.h>

#define CANVAS_WIDTH UINT32_C(340)
#define CANVAS_HEIGHT UINT32_C(220)
#define FRAME_NS UINT64_C(75000000)
#define RUN_NS UINT64_C(5000000000)

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

static void draw_dashboard(struct canvas *canvas)
{
    static const uint8_t chart[18] = {
        22U, 20U, 21U, 17U, 18U, 14U, 16U, 11U, 12U,
        9U, 11U, 7U, 8U, 4U, 6U, 3U, 4U, 1U
    };

    fill_rect(canvas, 0U, 0U, CANVAS_WIDTH, CANVAS_HEIGHT, 0x00101522U);
    fill_rect(canvas, 0U, 0U, CANVAS_WIDTH, 42U, 0x001A2435U);
    fill_rect(canvas, 18U, 62U, 194U, 138U, 0x00192230U);
    fill_rect(canvas, 226U, 62U, 96U, 64U, 0x00192230U);
    fill_rect(canvas, 226U, 136U, 96U, 64U, 0x00192230U);
    fill_rect(canvas, 18U, 62U, 4U, 138U, 0x0039D0B0U);
    draw_text(canvas, 18U, 14U, "SAPOTE CANVAS", 0x00F2F5F7U, 2U);
    draw_text(canvas, 232U, 18U, "ABI V1", 0x007F8DA3U, 1U);
    draw_text(canvas, 34U, 76U, "NATIVE SURFACE", 0x00B9C4D2U, 1U);
    draw_text(canvas, 238U, 76U, "INPUT", 0x00B9C4D2U, 1U);
    draw_text(canvas, 238U, 150U, "FOCUS", 0x00B9C4D2U, 1U);
    for (uint32_t index = 1U; index < 18U; ++index) {
        const uint32_t left = 35U + (index - 1U) * 9U;
        const uint32_t right = 35U + index * 9U;
        const uint32_t first = 183U - chart[index - 1U] * 3U;
        const uint32_t second = 183U - chart[index] * 3U;
        const uint32_t low = first < second ? first : second;
        const uint32_t high = first > second ? first : second;

        fill_rect(canvas, left, first, right - left + 1U, 2U, 0x0039D0B0U);
        fill_rect(canvas, right, low, 2U, high - low + 2U, 0x0039D0B0U);
    }
    draw_text(canvas, 34U, 185U, "DAMAGE RECTS, NOT FRAMES", 0x007F8DA3U,
        1U);
    fill_rect(canvas, 238U, 96U, 70U, 14U, 0x00273143U);
    fill_rect(canvas, 238U, 170U, 70U, 14U, 0x00273143U);
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
    uint64_t present_elapsed = 0U;
    int running = 1;

    (void)argc;
    (void)argv;
    (void)environment;
    if (sapote_window_create("Sapote Native Canvas", CANVAS_WIDTH,
            CANVAS_HEIGHT, &response) != 0 ||
        response.surface_address == 0U ||
        response.stride_bytes != CANVAS_WIDTH * sizeof(uint32_t)) {
        return 20;
    }
    canvas.pixels = (uint32_t *)(uintptr_t)response.surface_address;
    canvas.stride = response.stride_bytes / sizeof(uint32_t);
    draw_dashboard(&canvas);
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
                0x0039D0B0U : 0x005765FFU;
            uint64_t present_started;

            fill_rect(&canvas, 238U, 96U, 70U, 14U, 0x00273143U);
            fill_rect(&canvas, 238U, 96U, 10U + (pulse % 7U) * 10U, 14U,
                color);
            present_started = sapote_monotonic_ns();
            if (present(response.window, 238U, 96U, 70U, 14U) != 0) {
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
                const uint32_t color = event.value != 0U ?
                    0x0039D0B0U : 0x00636D7DU;
                ++focus_events;
                fill_rect(&canvas, 238U, 170U, 70U, 14U, color);
                if (present(response.window, 238U, 170U, 70U, 14U) != 0) {
                    return 24;
                }
                ++partial_presents;
            } else if (event.type == SAPOTE_EVENT_KEY &&
                event.value != SAPOTE_KEY_RELEASED) {
                ++key_events;
                fill_rect(&canvas, 0U, 0U, CANVAS_WIDTH, 42U,
                    (key_events & 1U) == 0U ? 0x001A2435U : 0x00242A45U);
                draw_text(&canvas, 18U, 14U, "SAPOTE CANVAS", 0x00F2F5F7U,
                    2U);
                draw_text(&canvas, 232U, 18U, "ABI V1", 0x007F8DA3U, 1U);
                if (present(response.window, 0U, 0U, CANVAS_WIDTH, 42U) !=
                        0) {
                    return 25;
                }
                ++partial_presents;
            } else if (event.type == SAPOTE_EVENT_POINTER_MOVE ||
                event.type == SAPOTE_EVENT_POINTER_BUTTON) {
                ++pointer_events;
                fill_rect(&canvas, 302U, 14U, 20U, 16U,
                    (pointer_events & 1U) == 0U ? 0x005765FFU : 0x00F0B84BU);
                if (event.type == SAPOTE_EVENT_POINTER_BUTTON) {
                    (void)sapote_pointer_capture(response.window,
                        event.value != 0U);
                }
                if (present(response.window, 302U, 14U, 20U, 16U) != 0) {
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
