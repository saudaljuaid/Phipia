/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/boot_ledger.h>
#include <sapote/cpu.h>
#include <sapote/framebuffer.h>
#include <sapote/heap.h>
#include <sapote/logo.h>
#include <sapote/memory.h>
#include <sapote/pci.h>
#include <sapote/pointer.h>
#include <sapote/screen.h>
#include <sapote/thread.h>
#include <sapote/ui.h>
#include <sapote/ui_font.h>

#define UI_MIN_WIDTH 800U
#define UI_MIN_HEIGHT 600U
#define UI_MAX_WIDTH 1920U
#define UI_MAX_HEIGHT 1200U
#define UI_LOGO_WIDTH 280U
#define UI_LOGO_HEIGHT 258U
#define UI_LOGO_PIXELS (UI_LOGO_WIDTH * UI_LOGO_HEIGHT)
#define UI_LOGO_BITMAP_SCALE 1U
#define UI_MENU_HEIGHT 24U
#define UI_WORKSPACE_MENU_WIDTH 132U
#define UI_WORKSPACE_MENU_HEIGHT 202U
#define UI_HERO_MAX_WIDTH 640U
#define UI_HERO_HEIGHT 388U
#define UI_HERO_TITLE_HEIGHT 24U
#define UI_DOCK_WIDTH 84U
#define UI_DOCK_ITEM_WIDTH 70U
#define UI_DOCK_ITEM_HEIGHT 62U
#define UI_DOCK_GAP 4U
#define UI_DOCK_PADDING 7U
#define UI_DOCK_HEIGHT (UI_DOCK_PADDING * 2U + \
    UI_DOCK_ITEM_COUNT * UI_DOCK_ITEM_HEIGHT + \
    (UI_DOCK_ITEM_COUNT - 1U) * UI_DOCK_GAP)
#define UI_FONT_ASCENT 12U
#define UI_FONT_DESCENT 4U
#define UI_FONT_ADVANCE 8U
#define UI_PANEL_TITLE_HEIGHT 26U

static const char label_terminal[] = "Terminal";
static const char label_ledger[] = "Ledger";
static const char label_system[] = "System";
static const char label_about[] = "About";

static struct ui_state state;
static struct surface *canvas;
static struct ui_event event_queue[UI_EVENT_QUEUE_CAPACITY];
static size_t event_count;
static uint32_t logo_pixels[UI_LOGO_PIXELS];
static const char *self_test_failure = "First Light UI self-test not run";
static const char *event_queue_failure = "UI event queue self-test not run";

static const uint16_t cursor_outer[UI_CURSOR_HEIGHT] = {
    0x800U, 0xC00U, 0xE00U, 0xF00U, 0xF80U, 0xFC0U,
    0xFE0U, 0xFF0U, 0xFF8U, 0xFC0U, 0xDC0U, 0xCE0U,
    0xC60U, 0x630U, 0x630U, 0x318U, 0x318U, 0x000U
};
static const uint16_t cursor_inner[UI_CURSOR_HEIGHT] = {
    0x000U, 0x400U, 0x600U, 0x700U, 0x780U, 0x7C0U,
    0x7E0U, 0x7F0U, 0x7C0U, 0x580U, 0x4C0U, 0x460U,
    0x420U, 0x210U, 0x210U, 0x108U, 0x000U, 0x000U
};

static bool add_u32(uint32_t left, uint32_t right, uint32_t *sum)
{
    if (sum == NULL || left > UINT32_MAX - right) {
        return false;
    }
    *sum = left + right;
    return true;
}

static bool rect_end(struct ui_rect rectangle, uint32_t *right, uint32_t *bottom)
{
    return rectangle.width != 0U && rectangle.height != 0U &&
        add_u32(rectangle.x, rectangle.width, right) &&
        add_u32(rectangle.y, rectangle.height, bottom);
}

static struct ui_rect drop_shadow_bounds(
    struct ui_rect rectangle,
    uint32_t offset
)
{
    uint32_t width;
    uint32_t height;

    if (!add_u32(rectangle.width, offset, &width) ||
        !add_u32(rectangle.height, offset, &height)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ rectangle.x, rectangle.y, width, height };
}

static struct ui_rect drop_shadow_draw_rect(
    struct ui_rect rectangle,
    uint32_t offset
)
{
    uint32_t x;
    uint32_t y;

    if (!add_u32(rectangle.x, offset, &x) ||
        !add_u32(rectangle.y, offset, &y)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return (struct ui_rect){ x, y, rectangle.width, rectangle.height };
}

static bool rect_contains_point(struct ui_rect rectangle, struct ui_point point)
{
    uint32_t right;
    uint32_t bottom;

    return point.x >= 0 && point.y >= 0 &&
        rect_end(rectangle, &right, &bottom) &&
        (uint32_t)point.x >= rectangle.x && (uint32_t)point.x < right &&
        (uint32_t)point.y >= rectangle.y && (uint32_t)point.y < bottom;
}

static bool rects_intersect(struct ui_rect left, struct ui_rect right)
{
    uint32_t left_right;
    uint32_t left_bottom;
    uint32_t right_right;
    uint32_t right_bottom;

    return rect_end(left, &left_right, &left_bottom) &&
        rect_end(right, &right_right, &right_bottom) &&
        left.x < right_right && right.x < left_right &&
        left.y < right_bottom && right.y < left_bottom;
}

static struct ui_rect rect_intersection(struct ui_rect left, struct ui_rect right)
{
    struct ui_rect result = { 0U, 0U, 0U, 0U };
    uint32_t left_right;
    uint32_t left_bottom;
    uint32_t right_right;
    uint32_t right_bottom;

    if (!rects_intersect(left, right) ||
        !rect_end(left, &left_right, &left_bottom) ||
        !rect_end(right, &right_right, &right_bottom)) {
        return result;
    }
    result.x = left.x > right.x ? left.x : right.x;
    result.y = left.y > right.y ? left.y : right.y;
    const uint32_t end_x = left_right < right_right ? left_right : right_right;
    const uint32_t end_y = left_bottom < right_bottom ? left_bottom : right_bottom;
    result.width = end_x - result.x;
    result.height = end_y - result.y;
    return result;
}

static struct ui_rect rect_union(struct ui_rect left, struct ui_rect right)
{
    uint32_t left_right;
    uint32_t left_bottom;
    uint32_t right_right;
    uint32_t right_bottom;

    if (left.width == 0U || left.height == 0U) {
        return right;
    }
    if (right.width == 0U || right.height == 0U) {
        return left;
    }
    if (!rect_end(left, &left_right, &left_bottom) ||
        !rect_end(right, &right_right, &right_bottom)) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    const uint32_t start_x = left.x < right.x ? left.x : right.x;
    const uint32_t start_y = left.y < right.y ? left.y : right.y;
    const uint32_t end_x = left_right > right_right ? left_right : right_right;
    const uint32_t end_y = left_bottom > right_bottom ? left_bottom : right_bottom;
    return (struct ui_rect){ start_x, start_y, end_x - start_x, end_y - start_y };
}

static struct surface_rect surface_rect_of(struct ui_rect rectangle)
{
    return (struct surface_rect){
        rectangle.x, rectangle.y, rectangle.width, rectangle.height
    };
}

static struct ui_rect dock_bounds_for(
    const struct ui_layout *layout,
    enum ui_element_id element
)
{
    if (layout == NULL || element <= UI_ELEMENT_NONE ||
        element >= UI_ELEMENT_COUNT) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    return layout->dock_items[(size_t)element - 1U].bounds;
}

static enum ui_panel_id panel_for_element(enum ui_element_id element)
{
    if (element <= UI_ELEMENT_NONE || element >= UI_ELEMENT_COUNT) {
        return UI_PANEL_NONE;
    }
    return state.layout.dock_items[(size_t)element - 1U].panel;
}

static bool baseline_fits(struct ui_rect rectangle, uint32_t baseline)
{
    uint32_t bottom;
    uint32_t right;

    return rect_end(rectangle, &right, &bottom) &&
        baseline >= rectangle.y + UI_FONT_ASCENT &&
        baseline <= UINT32_MAX - UI_FONT_DESCENT &&
        baseline + UI_FONT_DESCENT <= bottom;
}

enum ui_status ui_layout_build(
    uint32_t width,
    uint32_t height,
    struct ui_layout *layout
)
{
    uint32_t workspace_left;
    uint32_t workspace_right;
    uint32_t available_width;
    uint32_t hero_width;
    uint32_t logo_width;
    uint32_t logo_height;
    uint32_t hero_y;
    uint32_t panel_width;
    uint32_t panel_height;

    if (layout == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (width < UI_MIN_WIDTH || height < UI_MIN_HEIGHT ||
        width > UI_MAX_WIDTH || height > UI_MAX_HEIGHT) {
        return UI_STATUS_UNSUPPORTED_GEOMETRY;
    }
    workspace_left = UI_WORKSPACE_MENU_WIDTH + 32U;
    workspace_right = width - UI_DOCK_WIDTH - 16U;
    available_width = workspace_right - workspace_left;
    hero_width = available_width < UI_HERO_MAX_WIDTH ?
        available_width : UI_HERO_MAX_WIDTH;
    logo_width = hero_width < 600U ? 210U : UI_LOGO_WIDTH;
    logo_height =
        (logo_width * UI_LOGO_HEIGHT + UI_LOGO_WIDTH / 2U) / UI_LOGO_WIDTH;

    *layout = (struct ui_layout){ 0 };
    layout->surface = (struct ui_rect){ 0U, 0U, width, height };
    layout->menu_bar = (struct ui_rect){ 0U, 0U, width, UI_MENU_HEIGHT };
    layout->workspace_bar = (struct ui_rect){
        8U, UI_MENU_HEIGHT + 14U,
        UI_WORKSPACE_MENU_WIDTH, UI_WORKSPACE_MENU_HEIGHT
    };
    layout->menu_baseline = 17U;
    hero_y = height == UI_MIN_HEIGHT ? 48U : 60U;
    layout->hero_window = (struct ui_rect){
        workspace_left + (available_width - hero_width) / 2U, hero_y,
        hero_width, UI_HERO_HEIGHT
    };
    layout->hero_title_baseline = hero_y + 18U;
    layout->logo = (struct ui_rect){
        layout->hero_window.x + 20U, hero_y + UI_HERO_TITLE_HEIGHT + 34U,
        logo_width, logo_height
    };
    layout->wordmark = (struct ui_rect){
        layout->hero_window.x + hero_width - 286U, hero_y + 82U,
        246U, 16U
    };
    layout->title_baseline = layout->wordmark.y + UI_FONT_ASCENT;
    layout->motto = (struct ui_rect){
        layout->wordmark.x, layout->wordmark.y + 28U,
        246U, 16U
    };
    layout->motto_baseline = layout->motto.y + UI_FONT_ASCENT;
    layout->version_label = (struct ui_rect){
        layout->wordmark.x, layout->motto.y + 166U,
        246U, 20U
    };
    layout->version_baseline = layout->version_label.y + 15U;
    layout->dock = (struct ui_rect){
        width - UI_DOCK_WIDTH - 8U, UI_MENU_HEIGHT + 14U,
        UI_DOCK_WIDTH, UI_DOCK_HEIGHT
    };

    static const enum ui_element_id ids[UI_DOCK_ITEM_COUNT] = {
        UI_ELEMENT_DOCK_TERMINAL, UI_ELEMENT_DOCK_LEDGER,
        UI_ELEMENT_DOCK_SYSTEM, UI_ELEMENT_DOCK_ABOUT
    };
    static const char *const labels[UI_DOCK_ITEM_COUNT] = {
        label_terminal, label_ledger, label_system, label_about
    };
    static const enum ui_action actions[UI_DOCK_ITEM_COUNT] = {
        UI_ACTION_TOGGLE_TERMINAL, UI_ACTION_TOGGLE_LEDGER,
        UI_ACTION_TOGGLE_SYSTEM, UI_ACTION_TOGGLE_ABOUT
    };
    static const enum ui_panel_id panels[UI_DOCK_ITEM_COUNT] = {
        UI_PANEL_TERMINAL, UI_PANEL_LEDGER, UI_PANEL_SYSTEM, UI_PANEL_ABOUT
    };

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const uint32_t y = layout->dock.y + UI_DOCK_PADDING +
            (uint32_t)index * (UI_DOCK_ITEM_HEIGHT + UI_DOCK_GAP);

        layout->dock_items[index] = (struct ui_dock_item){
            .id = ids[index],
            .label = labels[index],
            .bounds = { layout->dock.x + UI_DOCK_PADDING, y,
                UI_DOCK_ITEM_WIDTH, UI_DOCK_ITEM_HEIGHT },
            .icon_bounds = { layout->dock.x + UI_DOCK_PADDING + 21U,
                y + 6U, 28U, 28U },
            .action = actions[index],
            .panel = panels[index]
        };
    }
    layout->dock_label_baseline = layout->dock_items[0].bounds.y + 55U;

    panel_width = width - UI_DOCK_WIDTH - 72U < 720U ?
        width - UI_DOCK_WIDTH - 72U : 720U;
    panel_height = height >= 720U ? 400U : 330U;
    layout->panel = (struct ui_rect){
        (width - UI_DOCK_WIDTH - panel_width) / 2U,
        (height - panel_height) / 2U,
        panel_width, panel_height
    };
    layout->panel_client = (struct ui_rect){
        layout->panel.x + 8U, layout->panel.y + 36U,
        panel_width - 16U, panel_height - 44U
    };
    layout->panel_title_baseline = layout->panel.y + 21U;
    layout->panel_text_baseline = layout->panel_client.y + UI_FONT_ASCENT;

    return ui_layout_validate(layout);
}

static enum ui_status validate_rect(
    struct ui_rect rectangle,
    struct ui_rect surface
)
{
    uint32_t right;
    uint32_t bottom;
    uint32_t surface_right;
    uint32_t surface_bottom;

    if (!rect_end(rectangle, &right, &bottom)) {
        return UI_STATUS_RECTANGLE_OVERFLOW;
    }
    if (!rect_end(surface, &surface_right, &surface_bottom)) {
        return UI_STATUS_RECTANGLE_OVERFLOW;
    }
    if (rectangle.x < surface.x || rectangle.y < surface.y ||
        right > surface_right || bottom > surface_bottom) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    return UI_STATUS_OK;
}

enum ui_status ui_layout_validate(const struct ui_layout *layout)
{
    if (layout == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (layout->surface.width < UI_MIN_WIDTH ||
        layout->surface.height < UI_MIN_HEIGHT) {
        return UI_STATUS_UNSUPPORTED_GEOMETRY;
    }

    const struct ui_rect rectangles[] = {
        layout->surface, layout->menu_bar, layout->workspace_bar,
        layout->hero_window, layout->logo, layout->wordmark, layout->motto,
        layout->version_label, layout->dock, layout->panel,
        layout->panel_client, drop_shadow_bounds(layout->hero_window, 7U),
        drop_shadow_bounds(layout->workspace_bar, 4U),
        drop_shadow_bounds(layout->dock, 6U),
        drop_shadow_bounds(layout->panel, 6U)
    };
    for (size_t index = 0U; index < sizeof(rectangles) / sizeof(rectangles[0]);
         ++index) {
        const enum ui_status status =
            validate_rect(rectangles[index], layout->surface);

        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    if (layout->panel_client.width == 0U ||
        layout->panel_client.height == 0U) {
        return UI_STATUS_EMPTY_PANEL_CLIENT;
    }

    bool seen[UI_ELEMENT_COUNT] = { false };
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const struct ui_dock_item *item = &layout->dock_items[index];

        if (item->id <= UI_ELEMENT_NONE || item->id >= UI_ELEMENT_COUNT ||
            item->action <= UI_ACTION_NONE || item->action >= UI_ACTION_COUNT ||
            item->panel <= UI_PANEL_NONE || item->panel >= UI_PANEL_COUNT ||
            item->label == NULL) {
            return UI_STATUS_BAD_ELEMENT;
        }
        if (seen[item->id]) {
            return UI_STATUS_DUPLICATE_ELEMENT_ID;
        }
        seen[item->id] = true;
        if (validate_rect(item->bounds, layout->dock) != UI_STATUS_OK ||
            validate_rect(item->icon_bounds, item->bounds) != UI_STATUS_OK) {
            return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
        }
        for (size_t other = index + 1U; other < UI_DOCK_ITEM_COUNT; ++other) {
            if (rects_intersect(item->bounds,
                    layout->dock_items[other].bounds)) {
                return UI_STATUS_DOCK_OVERLAP;
            }
        }
    }
    for (enum ui_element_id id = UI_ELEMENT_DOCK_TERMINAL;
         id < UI_ELEMENT_COUNT; id = (enum ui_element_id)(id + 1)) {
        if (!seen[id]) {
            return UI_STATUS_BAD_ELEMENT;
        }
    }

    if (!baseline_fits(layout->menu_bar, layout->menu_baseline) ||
        !baseline_fits(layout->hero_window, layout->hero_title_baseline) ||
        !baseline_fits(layout->wordmark, layout->title_baseline) ||
        !baseline_fits(layout->motto, layout->motto_baseline) ||
        !baseline_fits(layout->version_label, layout->version_baseline) ||
        !baseline_fits(layout->panel, layout->panel_title_baseline) ||
        !baseline_fits(layout->panel_client, layout->panel_text_baseline)) {
        return UI_STATUS_TEXT_BASELINE_OUT_OF_BOUNDS;
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (!baseline_fits(layout->dock_items[index].bounds,
                layout->dock_items[index].bounds.y + 55U)) {
            return UI_STATUS_TEXT_BASELINE_OUT_OF_BOUNDS;
        }
    }
    if (UI_CURSOR_HOTSPOT_X >= UI_CURSOR_WIDTH ||
        UI_CURSOR_HOTSPOT_Y >= UI_CURSOR_HEIGHT) {
        return UI_STATUS_BAD_CURSOR_HOTSPOT;
    }
    return UI_STATUS_OK;
}

enum ui_status ui_hit_test(
    const struct ui_layout *layout,
    struct ui_point point,
    enum ui_element_id *element
)
{
    enum ui_element_id hit = UI_ELEMENT_NONE;

    if (layout == NULL || element == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (!rect_contains_point(layout->dock_items[index].bounds, point)) {
            continue;
        }
        if (hit != UI_ELEMENT_NONE) {
            return UI_STATUS_HIT_TEST_AMBIGUOUS;
        }
        hit = layout->dock_items[index].id;
    }
    *element = hit;
    return UI_STATUS_OK;
}

static void install_theme(struct ui_theme *theme)
{
    theme->white = framebuffer_pack(0xF7U, 0xF6U, 0xF0U);
    theme->ink = framebuffer_pack(0x10U, 0x10U, 0x12U);
    theme->desktop_dark = framebuffer_pack(0x59U, 0x59U, 0x76U);
    theme->desktop_light = framebuffer_pack(0x66U, 0x66U, 0x84U);
    theme->title_active = framebuffer_pack(0x18U, 0x18U, 0x1CU);
    theme->title_inactive = framebuffer_pack(0x7AU, 0x7AU, 0x82U);
    theme->accent_teal = framebuffer_pack(0x4FU, 0x83U, 0x7FU);
    theme->accent_gold = framebuffer_pack(0xC4U, 0xA4U, 0x4EU);
    theme->accent_green = framebuffer_pack(0x59U, 0x85U, 0x61U);
    theme->accent_red = framebuffer_pack(0xA5U, 0x50U, 0x50U);
    theme->accent_violet = framebuffer_pack(0x70U, 0x59U, 0x84U);
    theme->shadow = framebuffer_pack(0x35U, 0x35U, 0x42U);
    theme->window_face = framebuffer_pack(0xD7U, 0xD6U, 0xCEU);
}

static enum ui_status fill_clipped(
    struct ui_rect rectangle,
    struct ui_rect damage,
    uint32_t pixel
)
{
    const struct ui_rect clipped = rect_intersection(rectangle, damage);

    if (clipped.width == 0U || clipped.height == 0U) {
        return UI_STATUS_OK;
    }
    return surface_fill_rect(canvas, surface_rect_of(clipped), pixel) ==
        SURFACE_STATUS_OK ? UI_STATUS_OK : UI_STATUS_SURFACE_FAILURE;
}

static enum ui_status stroke_clipped(
    struct ui_rect rectangle,
    struct ui_rect damage,
    uint32_t thickness,
    uint32_t pixel
)
{
    if (rectangle.width < thickness * 2U ||
        rectangle.height < thickness * 2U) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    const struct ui_rect edges[4] = {
        { rectangle.x, rectangle.y, rectangle.width, thickness },
        { rectangle.x, rectangle.y + rectangle.height - thickness,
            rectangle.width, thickness },
        { rectangle.x, rectangle.y, thickness, rectangle.height },
        { rectangle.x + rectangle.width - thickness, rectangle.y,
            thickness, rectangle.height }
    };
    for (size_t index = 0U; index < 4U; ++index) {
        const enum ui_status status = fill_clipped(edges[index], damage, pixel);

        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status bevel_clipped(
    struct ui_rect rectangle,
    struct ui_rect damage,
    bool raised
)
{
    if (rectangle.width < 2U || rectangle.height < 2U) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    const uint32_t upper = raised ? state.theme.white :
        state.theme.ink;
    const uint32_t lower = raised ? state.theme.ink :
        state.theme.white;
    const struct ui_rect upper_edges[2] = {
        { rectangle.x, rectangle.y, rectangle.width, 1U },
        { rectangle.x, rectangle.y, 1U, rectangle.height }
    };
    const struct ui_rect lower_edges[2] = {
        { rectangle.x, rectangle.y + rectangle.height - 1U,
            rectangle.width, 1U },
        { rectangle.x + rectangle.width - 1U, rectangle.y,
            1U, rectangle.height }
    };

    for (size_t index = 0U; index < 2U; ++index) {
        enum ui_status status = fill_clipped(upper_edges[index], damage,
            upper);

        if (status != UI_STATUS_OK) {
            return status;
        }
        status = fill_clipped(lower_edges[index], damage, lower);
        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status focus_mark_clipped(
    struct ui_rect rectangle,
    struct ui_rect damage,
    uint32_t pixel
)
{
    if (rectangle.width < 6U || rectangle.height < 6U) {
        return UI_STATUS_RECTANGLE_OUT_OF_BOUNDS;
    }
    for (uint32_t x = rectangle.x + 3U;
         x + 3U < rectangle.x + rectangle.width; x += 2U) {
        enum ui_status status = fill_clipped(
            (struct ui_rect){ x, rectangle.y + 3U, 1U, 1U }, damage, pixel);

        if (status != UI_STATUS_OK) {
            return status;
        }
        status = fill_clipped((struct ui_rect){ x,
            rectangle.y + rectangle.height - 4U, 1U, 1U }, damage, pixel);
        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    for (uint32_t y = rectangle.y + 5U;
         y + 5U < rectangle.y + rectangle.height; y += 2U) {
        enum ui_status status = fill_clipped(
            (struct ui_rect){ rectangle.x + 3U, y, 1U, 1U }, damage, pixel);

        if (status != UI_STATUS_OK) {
            return status;
        }
        status = fill_clipped((struct ui_rect){
            rectangle.x + rectangle.width - 4U, y, 1U, 1U }, damage, pixel);
        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_text(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t x,
    uint32_t baseline,
    const char *text,
    uint32_t pixel
)
{
    const struct ui_rect clip = rect_intersection(bounds, damage);
    size_t glyphs = 0U;

    if (clip.width == 0U || clip.height == 0U) {
        return UI_STATUS_OK;
    }
    if (ui_font_draw_text_clipped(canvas, surface_rect_of(bounds),
            surface_rect_of(clip), x, baseline, text, pixel, &glyphs) !=
        UI_FONT_STATUS_OK) {
        return UI_STATUS_FONT_FAILURE;
    }
    state.renders.glyphs += glyphs;
    return UI_STATUS_OK;
}

static enum ui_status draw_logo_bitmap(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t background
)
{
    static const uint8_t bayer_4x4[4][4] = {
        { 0U, 8U, 2U, 10U },
        { 12U, 4U, 14U, 6U },
        { 3U, 11U, 1U, 9U },
        { 15U, 7U, 13U, 5U }
    };
    const struct ui_rect clipped = rect_intersection(bounds, damage);
    const struct framebuffer_state framebuffer = framebuffer_get_state();

    if (clipped.width == 0U || clipped.height == 0U) {
        return UI_STATUS_OK;
    }
    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t target_x = clipped.x - bounds.x + x;
            const uint32_t target_y = clipped.y - bounds.y + y;
            const uint32_t source_x =
                target_x * UI_LOGO_WIDTH / bounds.width;
            const uint32_t source_y =
                target_y * UI_LOGO_HEIGHT / bounds.height;
            const uint32_t sample_x = source_x -
                source_x % UI_LOGO_BITMAP_SCALE;
            const uint32_t sample_y = source_y -
                source_y % UI_LOGO_BITMAP_SCALE;
            const uint32_t source = logo_pixels[
                sample_y * UI_LOGO_WIDTH + sample_x];
            const uint32_t red = (source >> framebuffer.red_position) & 0xFFU;
            const uint32_t green =
                (source >> framebuffer.green_position) & 0xFFU;
            const uint32_t blue =
                (source >> framebuffer.blue_position) & 0xFFU;
            const uint32_t luminance =
                (red * 77U + green * 150U + blue * 29U) >> 8U;
            uint32_t pixel = background;

            if (luminance <= 96U) {
                pixel = state.theme.ink;
            } else if (luminance < 248U) {
                const uint32_t coverage = 255U - luminance;
                const uint32_t threshold =
                    (uint32_t)bayer_4x4[target_y & 3U][target_x & 3U] *
                        16U + 8U;

                if (coverage > threshold) {
                    pixel = state.theme.ink;
                }
            }

            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    pixel) !=
                SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_logo_clipped(struct ui_rect damage)
{
    return draw_logo_bitmap(state.layout.logo, damage,
        state.theme.window_face);
}

static enum ui_status draw_icon(
    enum ui_element_id id,
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t pixel
)
{
    uint32_t fill = state.theme.title_active;

    if (id == UI_ELEMENT_DOCK_LEDGER) {
        fill = state.theme.accent_gold;
    } else if (id == UI_ELEMENT_DOCK_SYSTEM) {
        fill = state.theme.accent_teal;
    } else if (id == UI_ELEMENT_DOCK_ABOUT) {
        fill = state.theme.accent_violet;
    }

    enum ui_status status = fill_clipped(bounds, damage,
        state.theme.window_face);
    if (status == UI_STATUS_OK) {
        status = bevel_clipped(bounds, damage, true);
    }
    if (id == UI_ELEMENT_DOCK_TERMINAL) {
        const struct ui_rect screen = {
            bounds.x + 3U, bounds.y + 3U, 22U, 17U
        };
        status = fill_clipped(screen, damage, fill);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(screen, damage, 1U, pixel);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ bounds.x + 7U,
                bounds.y + 8U, 4U, 2U }, damage, state.theme.white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ bounds.x + 13U,
                bounds.y + 13U, 7U, 2U }, damage, state.theme.white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ bounds.x + 8U,
                bounds.y + 23U, 12U, 2U }, damage, pixel);
        }
    } else if (id == UI_ELEMENT_DOCK_LEDGER) {
        const struct ui_rect page = {
            bounds.x + 5U, bounds.y + 3U, 18U, 22U
        };
        status = fill_clipped(page, damage, state.theme.white);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(page, damage, 1U, pixel);
        }
        for (uint32_t row = 0U; row < 3U && status == UI_STATUS_OK; ++row) {
            status = fill_clipped((struct ui_rect){ bounds.x + 8U,
                bounds.y + 7U + row * 5U, 3U, 3U }, damage, fill);
            if (status == UI_STATUS_OK) {
                status = fill_clipped((struct ui_rect){ bounds.x + 13U,
                    bounds.y + 8U + row * 5U, 7U, 1U }, damage, pixel);
            }
        }
    } else if (id == UI_ELEMENT_DOCK_SYSTEM) {
        const struct ui_rect chip = {
            bounds.x + 5U, bounds.y + 5U, 18U, 18U
        };
        status = fill_clipped(chip, damage, fill);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(chip, damage, 1U, pixel);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ bounds.x + 10U,
                bounds.y + 10U, 8U, 8U }, damage, state.theme.white);
        }
        for (uint32_t pin = 0U; pin < 3U && status == UI_STATUS_OK; ++pin) {
            status = fill_clipped((struct ui_rect){ bounds.x + 2U,
                bounds.y + 8U + pin * 5U, 3U, 1U }, damage, pixel);
            if (status == UI_STATUS_OK) {
                status = fill_clipped((struct ui_rect){ bounds.x + 23U,
                    bounds.y + 8U + pin * 5U, 3U, 1U }, damage, pixel);
            }
        }
    } else if (id == UI_ELEMENT_DOCK_ABOUT) {
        const struct ui_rect card = {
            bounds.x + 4U, bounds.y + 4U, 20U, 20U
        };
        status = fill_clipped(card, damage, fill);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(card, damage, 1U, pixel);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ bounds.x + 13U,
                bounds.y + 8U, 3U, 3U }, damage, state.theme.white);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ bounds.x + 13U,
                bounds.y + 13U, 3U, 7U }, damage, state.theme.white);
        }
    }
    return status;
}

static uint32_t centered_text_x(struct ui_rect bounds, const char *text)
{
    uint32_t width = 0U;

    if (ui_font_text_width(text, &width) != UI_FONT_STATUS_OK ||
        width >= bounds.width) {
        return bounds.x;
    }
    return bounds.x + (bounds.width - width) / 2U;
}

static enum ui_status draw_window_title(
    struct ui_rect title,
    struct ui_rect damage,
    uint32_t baseline,
    const char *label,
    bool close_box
)
{
    const uint32_t label_x = centered_text_x(title, label);
    uint32_t label_width;
    enum ui_status status = ui_font_text_width(label, &label_width) ==
        UI_FONT_STATUS_OK ? UI_STATUS_OK : UI_STATUS_FONT_FAILURE;

    if (status == UI_STATUS_OK) {
        status = fill_clipped(title, damage, state.theme.window_face);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            title.x, title.y + title.height - 1U, title.width, 1U
        }, damage, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            title.x, title.y, title.width, 1U
        }, damage, state.theme.title_inactive);
    }
    for (uint32_t y = title.y + 4U;
         y + 4U < title.y + title.height && status == UI_STATUS_OK; y += 3U) {
        const uint32_t label_left = label_x > title.x + 6U ?
            label_x - 6U : title.x;
        const uint32_t label_right = label_x + label_width + 6U;

        if (label_left > title.x) {
            status = fill_clipped((struct ui_rect){ title.x, y,
                label_left - title.x, 1U }, damage, state.theme.ink);
        }
        if (status == UI_STATUS_OK && label_right < title.x + title.width) {
            status = fill_clipped((struct ui_rect){ label_right, y,
                title.x + title.width - label_right, 1U }, damage,
                state.theme.ink);
        }
    }
    if (close_box) {
        const struct ui_rect close = {
            title.x + 5U, title.y + 5U, 16U, 16U
        };

        status = fill_clipped(close, damage, state.theme.window_face);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(close, damage, 1U, state.theme.ink);
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){
                close.x + 4U, close.y + 4U, 8U, 8U
            }, damage, state.theme.ink);
        }
    }
    if (status == UI_STATUS_OK) {
        const struct ui_rect zoom = {
            title.x + title.width - 21U, title.y + 5U, 16U, 16U
        };
        status = fill_clipped(zoom, damage, state.theme.window_face);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(zoom, damage, 1U, state.theme.ink);
        }
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(title, damage, label_x, baseline, label,
            state.theme.ink);
    }
    return status;
}

static bool panel_is_active_for(enum ui_element_id element)
{
    return panel_for_element(element) == state.active_panel;
}

static enum ui_status draw_dock_item(
    const struct ui_dock_item *item,
    struct ui_rect damage
)
{
    const bool active = panel_is_active_for(item->id);
    const bool hovered = state.hover == item->id;
    const bool focused = state.focus == item->id;
    const bool pressed = state.pressed == item->id;
    uint32_t background = state.theme.window_face;
    uint32_t foreground = state.theme.ink;

    if (active) {
        background = state.theme.title_active;
        foreground = state.theme.white;
    } else if (pressed) {
        background = state.theme.shadow;
    } else if (hovered) {
        background = state.theme.white;
    }

    enum ui_status status = fill_clipped(item->bounds, damage, background);
    if (status == UI_STATUS_OK) {
        status = bevel_clipped(item->bounds, damage, !pressed && !active);
    }
    if (status == UI_STATUS_OK && focused) {
        status = focus_mark_clipped(item->bounds, damage,
            active ? state.theme.white : state.theme.accent_gold);
    }
    if (status == UI_STATUS_OK) {
        status = draw_icon(item->id, item->icon_bounds, damage, foreground);
    }
    if (status == UI_STATUS_OK) {
        const struct ui_rect label_bounds = {
            item->bounds.x + 3U, item->bounds.y + 37U,
            item->bounds.width - 6U, 22U
        };
        status = draw_text(label_bounds, damage,
            centered_text_x(label_bounds, item->label),
            item->bounds.y + 55U, item->label, foreground);
    }
    return status;
}

static size_t append_text(char *buffer, size_t capacity, size_t at, const char *text)
{
    if (buffer == NULL || text == NULL || capacity == 0U) {
        return at;
    }
    for (size_t index = 0U; text[index] != '\0' && at + 1U < capacity;
         ++index) {
        buffer[at++] = text[index];
    }
    buffer[at] = '\0';
    return at;
}

static size_t append_u64(char *buffer, size_t capacity, size_t at, uint64_t value)
{
    char reversed[24];
    size_t count = 0U;

    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(reversed));
    while (count > 0U && at + 1U < capacity) {
        buffer[at++] = reversed[--count];
    }
    buffer[at] = '\0';
    return at;
}

static size_t append_hex(char *buffer, size_t capacity, size_t at, uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    at = append_text(buffer, capacity, at, "0x");
    for (int shift = 60; shift >= 0 && at + 1U < capacity; shift -= 4) {
        buffer[at++] = digits[(value >> (unsigned)shift) & 0xFU];
    }
    buffer[at] = '\0';
    return at;
}

static enum ui_status draw_panel_line(
    struct ui_rect damage,
    uint32_t line,
    const char *text
)
{
    const uint32_t baseline = state.layout.panel_text_baseline + line * 20U;

    if (!baseline_fits(state.layout.panel_client, baseline)) {
        return UI_STATUS_TEXT_BASELINE_OUT_OF_BOUNDS;
    }
    return draw_text(state.layout.panel_client, damage,
        state.layout.panel_client.x, baseline, text, state.theme.ink);
}

static enum ui_status draw_ledger_panel(struct ui_rect damage)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    char line[96];
    size_t at;
    enum ui_status status;

    status = draw_panel_line(damage, 0U,
        ledger != NULL && !ledger->degraded ? "installed state / PASS" :
        "installed state / DEGRADED");
    if (status != UI_STATUS_OK || ledger == NULL) {
        return status;
    }

    at = append_text(line, sizeof(line), 0U, "plan ");
    at = append_u64(line, sizeof(line), at, ledger->planned_count);
    at = append_text(line, sizeof(line), at, " / receipts ");
    (void)append_u64(line, sizeof(line), at, ledger->receipt_count);
    status = draw_panel_line(damage, 1U, line);
    if (status != UI_STATUS_OK) {
        return status;
    }
    at = append_text(line, sizeof(line), 0U, "capabilities ");
    at = append_u64(line, sizeof(line), at,
        ledger->established_capability_count);
    at = append_text(line, sizeof(line), at, " / skips ");
    (void)append_u64(line, sizeof(line), at, ledger->optional_skip_count);
    status = draw_panel_line(damage, 2U, line);
    if (status != UI_STATUS_OK) {
        return status;
    }
    at = append_text(line, sizeof(line), 0U, "fingerprint ");
    (void)append_hex(line, sizeof(line), at, ledger->fingerprint);
    return draw_panel_line(damage, 3U, line);
}

static enum ui_status draw_system_panel(struct ui_rect damage)
{
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    char line[96];
    size_t at;
    enum ui_status status = draw_panel_line(damage, 0U,
        "cpu / x86_64 / one boot processor");

    if (status != UI_STATUS_OK) {
        return status;
    }
    status = draw_panel_line(damage, 1U,
        "memory / physical frames / 16 MiB kernel heap");
    if (status != UI_STATUS_OK) {
        return status;
    }
    at = append_text(line, sizeof(line), 0U, "pci / ");
    at = append_u64(line, sizeof(line), at, pci_function_count());
    (void)append_text(line, sizeof(line), at, " functions");
    status = draw_panel_line(damage, 2U, line);
    if (status != UI_STATUS_OK) {
        return status;
    }
    status = draw_panel_line(damage, 3U,
        "timers / local APIC / TSC / ACPI PM");
    if (status != UI_STATUS_OK) {
        return status;
    }
    at = append_text(line, sizeof(line), 0U, "framebuffer / ");
    at = append_u64(line, sizeof(line), at, framebuffer.width);
    at = append_text(line, sizeof(line), at, " x ");
    at = append_u64(line, sizeof(line), at, framebuffer.height);
    (void)append_text(line, sizeof(line), at, " / cached WC surface");
    return draw_panel_line(damage, 4U, line);
}

static enum ui_status draw_about_panel(struct ui_rect damage)
{
    static const char *const lines[] = {
        "Sapote 1.1.0",
        "First Light / Pebble",
        "proof-driven operating system",
        "hello from the metal."
    };

    for (size_t index = 0U; index < sizeof(lines) / sizeof(lines[0]); ++index) {
        const enum ui_status status =
            draw_panel_line(damage, (uint32_t)index, lines[index]);

        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_panel(struct ui_rect damage)
{
    enum ui_status status;

    if (state.active_panel == UI_PANEL_NONE) {
        return UI_STATUS_OK;
    }
    status = fill_clipped(drop_shadow_draw_rect(state.layout.panel, 6U),
        damage, state.theme.shadow);
    if (status == UI_STATUS_OK) {
        status = fill_clipped(state.layout.panel, damage,
            state.theme.window_face);
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(state.layout.panel, damage, 1U,
            state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = bevel_clipped((struct ui_rect){ state.layout.panel.x + 1U,
            state.layout.panel.y + 1U, state.layout.panel.width - 2U,
            state.layout.panel.height - 2U }, damage, true);
    }
    if (status == UI_STATUS_OK) {
        status = draw_window_title((struct ui_rect){
            state.layout.panel.x + 4U, state.layout.panel.y + 4U,
            state.layout.panel.width - 8U, UI_PANEL_TITLE_HEIGHT
        }, damage, state.layout.panel_title_baseline,
            ui_panel_name(state.active_panel), true);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(state.layout.panel_client, damage,
            state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = bevel_clipped(state.layout.panel_client, damage, false);
    }
    if (status != UI_STATUS_OK) {
        return status;
    }

    if (state.active_panel == UI_PANEL_TERMINAL) {
        const struct ui_rect clip =
            rect_intersection(state.layout.panel_client, damage);

        if (clip.width != 0U && clip.height != 0U &&
            screen_redraw_region(surface_rect_of(clip)) != SCREEN_STATUS_OK) {
            return UI_STATUS_SCREEN_FAILURE;
        }
        return UI_STATUS_OK;
    }
    if (state.active_panel == UI_PANEL_LEDGER) {
        return draw_ledger_panel(damage);
    }
    if (state.active_panel == UI_PANEL_SYSTEM) {
        return draw_system_panel(damage);
    }
    if (state.active_panel == UI_PANEL_ABOUT) {
        return draw_about_panel(damage);
    }
    return UI_STATUS_BAD_PANEL;
}

static struct ui_rect cursor_rect_for(struct ui_point point)
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    if (point.x < 0 || point.y < 0 || state.layout.surface.width == 0U ||
        state.layout.surface.height == 0U) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    x = (uint32_t)point.x;
    y = (uint32_t)point.y;
    if (x >= state.layout.surface.width) {
        x = state.layout.surface.width - 1U;
    }
    if (y >= state.layout.surface.height) {
        y = state.layout.surface.height - 1U;
    }
    width = UI_CURSOR_WIDTH;
    height = UI_CURSOR_HEIGHT;
    if (width > state.layout.surface.width - x) {
        width = state.layout.surface.width - x;
    }
    if (height > state.layout.surface.height - y) {
        height = state.layout.surface.height - y;
    }
    return (struct ui_rect){ x, y, width, height };
}

static enum ui_status draw_cursor(struct ui_rect damage)
{
    const struct ui_rect cursor = cursor_rect_for(state.pointer);

    if (!state.pointer_present || !rects_intersect(cursor, damage)) {
        return UI_STATUS_OK;
    }
    for (uint32_t y = 0U; y < cursor.height; ++y) {
        for (uint32_t x = 0U; x < cursor.width; ++x) {
            const uint16_t bit = (uint16_t)(0x800U >> x);
            uint32_t pixel;

            if ((cursor_outer[y] & bit) == 0U) {
                continue;
            }
            pixel = (cursor_inner[y] & bit) != 0U ?
                state.theme.white : state.theme.ink;
            if (surface_pixel(canvas, cursor.x + x, cursor.y + y, pixel) !=
                SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status draw_desktop_pattern(struct ui_rect damage)
{
    enum ui_status status = fill_clipped(state.layout.surface, damage,
        state.theme.desktop_dark);
    const uint32_t start = state.layout.menu_bar.y +
        state.layout.menu_bar.height;

    for (uint32_t y = start; y < state.layout.surface.height &&
         status == UI_STATUS_OK; y += 2U) {
        status = fill_clipped((struct ui_rect){ 0U, y,
            state.layout.surface.width, 1U }, damage,
            state.theme.desktop_light);
    }
    return status;
}

static enum ui_status draw_workspace_menu(struct ui_rect damage)
{
    static const char *const rows[] = {
        "Info", "File", "Edit", "Disk", "View", "Tools", "Windows",
        "Services"
    };
    const struct ui_rect menu = state.layout.workspace_bar;
    const struct ui_rect header = {
        menu.x + 2U, menu.y + 2U, menu.width - 4U, 21U
    };
    enum ui_status status = fill_clipped(drop_shadow_draw_rect(menu, 4U),
        damage, state.theme.shadow);

    if (status == UI_STATUS_OK) {
        status = fill_clipped(menu, damage, state.theme.window_face);
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(menu, damage, 1U, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = bevel_clipped((struct ui_rect){ menu.x + 1U, menu.y + 1U,
            menu.width - 2U, menu.height - 2U }, damage, true);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(header, damage, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(header, damage,
            centered_text_x(header, "Workspace"), header.y + 15U,
            "Workspace", state.theme.white);
    }
    for (size_t index = 0U;
         index < sizeof(rows) / sizeof(rows[0]) && status == UI_STATUS_OK;
         ++index) {
        const struct ui_rect row = {
            menu.x + 3U, menu.y + 24U + (uint32_t)index * 21U,
            menu.width - 6U, 21U
        };

        status = fill_clipped(row, damage, state.theme.window_face);
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ row.x,
                row.y + row.height - 1U, row.width, 1U }, damage,
                state.theme.shadow);
        }
        if (status == UI_STATUS_OK) {
            status = draw_text(row, damage, row.x + 5U, row.y + 15U,
                rows[index], state.theme.ink);
        }
        for (uint32_t arrow = 0U; arrow < 3U && status == UI_STATUS_OK;
             ++arrow) {
            status = fill_clipped((struct ui_rect){
                row.x + row.width - 8U + arrow,
                row.y + 9U - arrow, 1U, arrow * 2U + 1U
            }, damage, state.theme.ink);
        }
    }
    return status;
}

static enum ui_status draw_hero_details(struct ui_rect damage)
{
    enum ui_status status = draw_text(state.layout.wordmark, damage,
        state.layout.wordmark.x, state.layout.title_baseline,
        "Sapote", state.theme.ink);

    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.motto, damage,
            state.layout.motto.x, state.layout.motto_baseline,
            "First Light", state.theme.title_active);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){ state.layout.motto.x,
            state.layout.motto.y + state.layout.motto.height + 6U,
            state.layout.motto.width, 1U }, damage, state.theme.shadow);
    }
    static const char *const lines[] = {
        "A small operating system,",
        "built from first principles.",
        "Choose a tool at right or",
        "press Tab to explore."
    };
    static const uint32_t offsets[] = { 54U, 74U, 114U, 134U };

    for (size_t index = 0U; index < 4U && status == UI_STATUS_OK; ++index) {
        status = draw_text((struct ui_rect){
                state.layout.wordmark.x,
                state.layout.motto.y + offsets[index] - UI_FONT_ASCENT,
                state.layout.wordmark.width,
                UI_FONT_ASCENT + UI_FONT_DESCENT
            }, damage, state.layout.wordmark.x,
            state.layout.motto.y + offsets[index], lines[index],
            state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.version_label, damage,
            state.layout.version_label.x, state.layout.version_baseline,
            "Version 1.1.0", state.theme.ink);
    }
    return status;
}

static enum ui_status render_region(struct ui_rect damage, bool full)
{
    enum ui_status status = draw_desktop_pattern(damage);

    if (status == UI_STATUS_OK) {
        status = fill_clipped(state.layout.menu_bar, damage,
            state.theme.window_face);
    }
    if (status == UI_STATUS_OK) {
        status = bevel_clipped(state.layout.menu_bar, damage, true);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            state.layout.menu_bar.x,
            state.layout.menu_bar.y + state.layout.menu_bar.height - 1U,
            state.layout.menu_bar.width, 1U
        }, damage, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 10U,
            state.layout.menu_baseline,
            "Sapote   File   Edit   View   Special   Window",
            state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_workspace_menu(damage);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(drop_shadow_draw_rect(
            state.layout.hero_window, 7U), damage, state.theme.shadow);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(state.layout.hero_window, damage,
            state.theme.window_face);
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(state.layout.hero_window, damage, 1U,
            state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = bevel_clipped((struct ui_rect){
            state.layout.hero_window.x + 1U,
            state.layout.hero_window.y + 1U,
            state.layout.hero_window.width - 2U,
            state.layout.hero_window.height - 2U
        }, damage, true);
    }
    if (status == UI_STATUS_OK) {
        status = draw_window_title((struct ui_rect){
            state.layout.hero_window.x + 2U,
            state.layout.hero_window.y + 2U,
            state.layout.hero_window.width - 4U,
            UI_HERO_TITLE_HEIGHT
        }, damage, state.layout.hero_title_baseline,
            "Welcome to Sapote", false);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            state.layout.wordmark.x - 16U,
            state.layout.hero_window.y + 42U,
            2U, state.layout.hero_window.height - 56U
        }, damage, state.theme.shadow);
    }

    if (status == UI_STATUS_OK) {
        status = draw_logo_clipped(damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_hero_details(damage);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(drop_shadow_draw_rect(state.layout.dock, 6U),
            damage, state.theme.shadow);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(state.layout.dock, damage,
            state.theme.window_face);
    }
    if (status == UI_STATUS_OK) {
        status = stroke_clipped(state.layout.dock, damage, 1U,
            state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = bevel_clipped((struct ui_rect){ state.layout.dock.x + 1U,
            state.layout.dock.y + 1U, state.layout.dock.width - 2U,
            state.layout.dock.height - 2U }, damage, true);
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT &&
         status == UI_STATUS_OK; ++index) {
        status = draw_dock_item(&state.layout.dock_items[index], damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_panel(damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_cursor(damage);
    }
    if (status != UI_STATUS_OK) {
        return status;
    }
    if (surface_present(canvas) != SURFACE_STATUS_OK) {
        return UI_STATUS_SURFACE_FAILURE;
    }
    if (full) {
        state.renders.full_draws += 1U;
    } else {
        state.renders.damaged_draws += 1U;
    }
    state.renders.damage_rectangles += 1U;
    state.renders.pixels_copied += canvas->last_present_pixels;
    return UI_STATUS_OK;
}

static uint64_t surface_hash(void)
{
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    const size_t count = (size_t)canvas->width * (size_t)canvas->height;

    for (size_t index = 0U; index < count; ++index) {
        hash ^= canvas->pixels[index];
        hash *= UINT64_C(0x100000001B3);
    }
    return hash;
}

enum ui_status ui_construct(bool pointer_present)
{
    uint32_t logo_width;
    uint32_t logo_height;
    const struct framebuffer_state framebuffer = framebuffer_get_state();

    if (state.initialized) {
        return UI_STATUS_ALREADY_INITIALIZED;
    }
    if (!framebuffer_is_active() || !screen_is_active() ||
        !ui_font_is_verified()) {
        return UI_STATUS_NOT_INITIALIZED;
    }
    canvas = screen_surface();
    if (canvas == NULL) {
        return UI_STATUS_SURFACE_FAILURE;
    }
    enum ui_status status = ui_layout_build(framebuffer.width,
        framebuffer.height, &state.layout);

    if (status != UI_STATUS_OK) {
        canvas = NULL;
        return status;
    }
    if (sapote_logo_geometry(&logo_width, &logo_height) != LOGO_STATUS_OK ||
        logo_width != UI_LOGO_WIDTH || logo_height != UI_LOGO_HEIGHT ||
        sapote_logo_decode(logo_pixels, UI_LOGO_PIXELS,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position, framebuffer_pack(0xFFU, 0xFFU, 0xFFU)) !=
            LOGO_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_LOGO_FAILURE;
    }

    install_theme(&state.theme);
    state.pointer_present = pointer_present;
    state.focus = UI_ELEMENT_DOCK_TERMINAL;
    state.hover = UI_ELEMENT_NONE;
    state.pressed = UI_ELEMENT_NONE;
    state.active_panel = UI_PANEL_NONE;
    state.ledger_pass = false;
    event_count = 0U;

    if (pointer_present) {
        if (pointer_set_bounds(framebuffer.width, framebuffer.height) !=
            POINTER_STATUS_OK) {
            canvas = NULL;
            return UI_STATUS_BAD_CURSOR_HOTSPOT;
        }
        const struct pointer_state pointer = pointer_get_state();
        state.pointer = (struct ui_point){
            (int32_t)pointer.x, (int32_t)pointer.y
        };
    } else {
        state.pointer = (struct ui_point){ 0, 0 };
    }

    if (screen_set_deferred_present(true) != SCREEN_STATUS_OK ||
        screen_set_visible(false) != SCREEN_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_SCREEN_FAILURE;
    }
    state.initialized = true;
    return UI_STATUS_OK;
}

enum ui_status ui_activate(void)
{
    if (!state.initialized) {
        return UI_STATUS_NOT_INITIALIZED;
    }
    if (state.active) {
        return UI_STATUS_ALREADY_INITIALIZED;
    }
    state.active = true;
    const enum ui_status status = render_region(state.layout.surface, true);

    if (status != UI_STATUS_OK) {
        state.active = false;
        return status;
    }
    state.stable_render_hash = surface_hash();
    return UI_STATUS_OK;
}

bool ui_is_active(void)
{
    return state.active;
}

const struct ui_state *ui_get_state(void)
{
    return &state;
}

static enum ui_status publish_unlocked(const struct ui_event *event)
{
    if (event == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (event->type <= UI_EVENT_NONE || event->type >= UI_EVENT_TYPE_COUNT) {
        return UI_STATUS_BAD_EVENT;
    }

    state.events.accepted += 1U;
    if (event->type == UI_EVENT_POINTER_MOVEMENT && event_count > 0U &&
        event_queue[event_count - 1U].type == UI_EVENT_POINTER_MOVEMENT) {
        event_queue[event_count - 1U] = *event;
        state.events.coalesced += 1U;
        return UI_STATUS_OK;
    }
    if (event_count == UI_EVENT_QUEUE_CAPACITY) {
        if (event->type == UI_EVENT_POINTER_MOVEMENT) {
            state.events.dropped += 1U;
            return UI_STATUS_EVENT_QUEUE_FULL;
        }
        size_t movement = event_count;
        for (size_t index = 0U; index < event_count; ++index) {
            if (event_queue[index].type == UI_EVENT_POINTER_MOVEMENT) {
                movement = index;
                break;
            }
        }
        if (movement == event_count) {
            state.events.dropped += 1U;
            return UI_STATUS_EVENT_QUEUE_FULL;
        }
        for (size_t index = movement + 1U; index < event_count; ++index) {
            event_queue[index - 1U] = event_queue[index];
        }
        event_count -= 1U;
        state.events.dropped += 1U;
    }
    event_queue[event_count++] = *event;
    return UI_STATUS_OK;
}

enum ui_status ui_event_publish(const struct ui_event *event)
{
    const bool enabled = cpu_interrupts_enabled();

    if (enabled) {
        cpu_interrupt_disable();
    }
    const enum ui_status status = publish_unlocked(event);
    if (enabled) {
        cpu_interrupt_enable();
    }
    return status;
}

static bool pop_event(struct ui_event *event)
{
    const bool enabled = cpu_interrupts_enabled();

    if (enabled) {
        cpu_interrupt_disable();
    }
    if (event_count == 0U) {
        if (enabled) {
            cpu_interrupt_enable();
        }
        return false;
    }
    *event = event_queue[0];
    for (size_t index = 1U; index < event_count; ++index) {
        event_queue[index - 1U] = event_queue[index];
    }
    event_count -= 1U;
    state.events.drained += 1U;
    if (enabled) {
        cpu_interrupt_enable();
    }
    return true;
}

enum ui_status ui_handle_keyboard(const struct keyboard_event *event)
{
    struct ui_event ui_event = { 0 };

    if (event == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (!state.active || !event->pressed) {
        return UI_STATUS_OK;
    }
    if (event->scancode == 0x0FU) {
        ui_event.type = event->shift ? UI_EVENT_KEYBOARD_FOCUS_PREVIOUS :
            UI_EVENT_KEYBOARD_FOCUS_NEXT;
    } else if (event->scancode == 0x1CU) {
        ui_event.type = UI_EVENT_KEYBOARD_ACTIVATION;
    } else if (event->scancode == 0x01U) {
        ui_event.type = UI_EVENT_PANEL_CLOSE;
    } else {
        return UI_STATUS_OK;
    }
    return ui_event_publish(&ui_event);
}

static enum ui_element_id next_focus(enum ui_element_id current, bool previous)
{
    if (current <= UI_ELEMENT_NONE || current >= UI_ELEMENT_COUNT) {
        return UI_ELEMENT_DOCK_TERMINAL;
    }
    if (previous) {
        return current == UI_ELEMENT_DOCK_TERMINAL ? UI_ELEMENT_DOCK_ABOUT :
            (enum ui_element_id)(current - 1);
    }
    return current == UI_ELEMENT_DOCK_ABOUT ? UI_ELEMENT_DOCK_TERMINAL :
        (enum ui_element_id)(current + 1);
}

static enum ui_status set_panel(
    enum ui_panel_id panel,
    struct ui_rect *damage
)
{
    const enum ui_panel_id old_panel = state.active_panel;

    if (panel < UI_PANEL_NONE || panel >= UI_PANEL_COUNT || damage == NULL) {
        return UI_STATUS_BAD_PANEL;
    }
    if (old_panel == panel) {
        panel = UI_PANEL_NONE;
    }
    if (old_panel != UI_PANEL_NONE || panel != UI_PANEL_NONE) {
        *damage = rect_union(*damage,
            drop_shadow_bounds(state.layout.panel, 6U));
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (state.layout.dock_items[index].panel == old_panel ||
            state.layout.dock_items[index].panel == panel) {
            *damage = rect_union(*damage,
                state.layout.dock_items[index].bounds);
        }
    }
    state.active_panel = panel;
    state.renders.panel_transitions += 1U;

    if (panel == UI_PANEL_TERMINAL) {
        if (screen_set_viewport(surface_rect_of(state.layout.panel_client),
                true) != SCREEN_STATUS_OK) {
            return UI_STATUS_SCREEN_FAILURE;
        }
    } else if (screen_set_visible(false) != SCREEN_STATUS_OK) {
        return UI_STATUS_SCREEN_FAILURE;
    }
    return UI_STATUS_OK;
}

static enum ui_status apply_event(
    const struct ui_event *event,
    struct ui_rect *damage
)
{
    enum ui_element_id hit = UI_ELEMENT_NONE;

    if (event == NULL || damage == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (event->type == UI_EVENT_POINTER_MOVEMENT) {
        const struct ui_rect old_cursor = cursor_rect_for(state.pointer);
        const enum ui_element_id old_hover = state.hover;

        state.pointer = event->point;
        const struct ui_rect new_cursor = cursor_rect_for(state.pointer);
        *damage = rect_union(*damage, rect_union(old_cursor, new_cursor));
        if (ui_hit_test(&state.layout, state.pointer, &hit) != UI_STATUS_OK) {
            return UI_STATUS_HIT_TEST_AMBIGUOUS;
        }
        state.hover = hit;
        state.renders.cursor_moves += 1U;
        if (old_hover != hit) {
            *damage = rect_union(*damage,
                dock_bounds_for(&state.layout, old_hover));
            *damage = rect_union(*damage,
                dock_bounds_for(&state.layout, hit));
            state.renders.dock_state_changes += 1U;
        }
    } else if (event->type == UI_EVENT_POINTER_BUTTON_PRESS &&
        event->button == UI_POINTER_BUTTON_LEFT) {
        if (ui_hit_test(&state.layout, event->point, &hit) != UI_STATUS_OK) {
            return UI_STATUS_HIT_TEST_AMBIGUOUS;
        }
        state.pressed = hit;
        *damage = rect_union(*damage, dock_bounds_for(&state.layout, hit));
        state.renders.dock_state_changes += 1U;
    } else if (event->type == UI_EVENT_POINTER_BUTTON_RELEASE &&
        event->button == UI_POINTER_BUTTON_LEFT) {
        const enum ui_element_id pressed = state.pressed;

        if (ui_hit_test(&state.layout, event->point, &hit) != UI_STATUS_OK) {
            return UI_STATUS_HIT_TEST_AMBIGUOUS;
        }
        state.pressed = UI_ELEMENT_NONE;
        *damage = rect_union(*damage,
            dock_bounds_for(&state.layout, pressed));
        if (pressed != UI_ELEMENT_NONE && pressed == hit) {
            const enum ui_status status =
                set_panel(panel_for_element(pressed), damage);

            if (status != UI_STATUS_OK) {
                return status;
            }
        }
        state.renders.dock_state_changes += 1U;
    } else if (event->type == UI_EVENT_KEYBOARD_FOCUS_NEXT ||
        event->type == UI_EVENT_KEYBOARD_FOCUS_PREVIOUS) {
        const enum ui_element_id old_focus = state.focus;

        state.focus = next_focus(state.focus,
            event->type == UI_EVENT_KEYBOARD_FOCUS_PREVIOUS);
        *damage = rect_union(*damage,
            dock_bounds_for(&state.layout, old_focus));
        *damage = rect_union(*damage,
            dock_bounds_for(&state.layout, state.focus));
        state.renders.dock_state_changes += 1U;
    } else if (event->type == UI_EVENT_KEYBOARD_ACTIVATION) {
        return set_panel(panel_for_element(state.focus), damage);
    } else if (event->type == UI_EVENT_PANEL_CLOSE) {
        if (state.active_panel != UI_PANEL_NONE) {
            return set_panel(UI_PANEL_NONE, damage);
        }
    } else if (event->type == UI_EVENT_REDRAW_REQUEST) {
        *damage = state.layout.surface;
    } else if (event->type != UI_EVENT_POINTER_BUTTON_PRESS &&
        event->type != UI_EVENT_POINTER_BUTTON_RELEASE) {
        return UI_STATUS_BAD_EVENT;
    }
    return UI_STATUS_OK;
}

enum ui_status ui_process_events(void)
{
    struct ui_event event;
    struct ui_rect damage = { 0U, 0U, 0U, 0U };

    if (!state.active) {
        return UI_STATUS_NOT_ACTIVE;
    }
    while (pop_event(&event)) {
        const enum ui_status status = apply_event(&event, &damage);

        if (status != UI_STATUS_OK) {
            return status;
        }
    }
    if (damage.width == 0U || damage.height == 0U) {
        return UI_STATUS_OK;
    }
    return render_region(damage,
        damage.x == 0U && damage.y == 0U &&
        damage.width == state.layout.surface.width &&
        damage.height == state.layout.surface.height);
}

enum ui_status ui_flush(void)
{
    struct ui_rect damage = { 0U, 0U, 0U, 0U };
    const struct boot_ledger *ledger;

    if (!state.active) {
        return UI_STATUS_NOT_ACTIVE;
    }
    ledger = boot_ledger_installed();
    const bool pass = ledger != NULL && ledger->executed && !ledger->degraded &&
        boot_ledger_fingerprint_valid(ledger);
    if (state.ledger_pass != pass) {
        state.ledger_pass = pass;
        if (state.active_panel == UI_PANEL_LEDGER) {
            damage = state.layout.panel;
        }
    }
    if (damage.width != 0U && damage.height != 0U) {
        return render_region(damage, false);
    }
    if (canvas->damage.pending) {
        const struct surface_rect pending = canvas->damage.rectangle;
        const struct ui_rect pending_ui = {
            pending.x, pending.y, pending.width, pending.height
        };
        const enum ui_status status = draw_cursor(pending_ui);

        if (status != UI_STATUS_OK ||
            surface_present(canvas) != SURFACE_STATUS_OK) {
            return UI_STATUS_SURFACE_FAILURE;
        }
        state.renders.damaged_draws += 1U;
        state.renders.damage_rectangles += 1U;
        state.renders.pixels_copied += canvas->last_present_pixels;
    }
    return UI_STATUS_OK;
}

static uint64_t synthetic_render_hash(bool active)
{
    uint32_t pixels[64U * 32U];
    const uint32_t ink = UINT32_C(0x00101012);
    const uint32_t desktop_dark = UINT32_C(0x00595976);
    const uint32_t desktop_light = UINT32_C(0x00666684);
    const uint32_t title_active = UINT32_C(0x0018181C);
    const uint32_t accent_gold = UINT32_C(0x00C4A44E);
    const uint32_t window_face = UINT32_C(0x00D7D6CE);

    for (size_t index = 0U; index < sizeof(pixels) / sizeof(pixels[0]); ++index) {
        pixels[index] = desktop_dark;
    }
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 64U; ++x) {
            pixels[y * 64U + x] = title_active;
        }
    }
    for (uint32_t y = 8U; y < 32U; y += 2U) {
        for (uint32_t x = 0U; x < 64U; ++x) {
            pixels[y * 64U + x] = desktop_light;
        }
    }
    for (uint32_t y = 18U; y < 30U; ++y) {
        for (uint32_t x = 8U; x < 56U; ++x) {
            const bool edge = y == 18U || y == 29U || x == 8U || x == 55U;
            pixels[y * 64U + x] = edge ? ink : window_face;
        }
    }
    for (uint32_t y = 22U; y < 28U; ++y) {
        for (uint32_t x = 12U; x < 22U; ++x) {
            pixels[y * 64U + x] = active ? title_active : accent_gold;
        }
    }
    for (uint32_t y = 0U; y < UI_CURSOR_HEIGHT && y + 4U < 32U; ++y) {
        for (uint32_t x = 0U; x < UI_CURSOR_WIDTH && x + 44U < 64U; ++x) {
            if ((cursor_outer[y] & (uint16_t)(0x800U >> x)) != 0U) {
                pixels[(y + 4U) * 64U + x + 44U] = ink;
            }
        }
    }

    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    for (size_t index = 0U; index < sizeof(pixels) / sizeof(pixels[0]); ++index) {
        hash ^= pixels[index];
        hash *= UINT64_C(0x100000001B3);
    }
    return hash;
}

static bool event_queue_self_test(void)
{
    const struct ui_event_counters saved = state.events;
    struct ui_event saved_queue[UI_EVENT_QUEUE_CAPACITY];
    const size_t saved_count = event_count;

    event_queue_failure = "UI event queue self-test passed";
    for (size_t index = 0U; index < saved_count; ++index) {
        saved_queue[index] = event_queue[index];
    }
    state.events = (struct ui_event_counters){ 0 };
    event_count = 0U;

    struct ui_event move = {
        .type = UI_EVENT_POINTER_MOVEMENT,
        .point = { 1, 1 },
        .button = UI_POINTER_BUTTON_NONE
    };
    if (publish_unlocked(&move) != UI_STATUS_OK) {
        event_queue_failure = "pointer movement event was refused by an empty queue";
        return false;
    }
    move.point.x = 2;
    if (publish_unlocked(&move) != UI_STATUS_OK || event_count != 1U ||
        state.events.coalesced != 1U) {
        event_queue_failure = "pointer movement did not coalesce in the fixed event queue";
        return false;
    }
    const struct ui_event activate = {
        .type = UI_EVENT_KEYBOARD_ACTIVATION,
        .point = { 0, 0 },
        .button = UI_POINTER_BUTTON_NONE
    };
    while (event_count < UI_EVENT_QUEUE_CAPACITY) {
        if (publish_unlocked(&activate) != UI_STATUS_OK) {
            event_queue_failure = "keyboard activation did not fill the event queue exactly";
            return false;
        }
    }
    const struct ui_event button = {
        .type = UI_EVENT_POINTER_BUTTON_PRESS,
        .point = { 2, 2 },
        .button = UI_POINTER_BUTTON_LEFT
    };
    if (publish_unlocked(&button) != UI_STATUS_OK ||
        event_queue[event_count - 1U].type != UI_EVENT_POINTER_BUTTON_PRESS ||
        state.events.dropped != 1U) {
        event_queue_failure = "button transition did not evict movement from the full event queue";
        return false;
    }

    event_count = saved_count;
    for (size_t index = 0U; index < saved_count; ++index) {
        event_queue[index] = saved_queue[index];
    }
    state.events = saved;
    return true;
}

bool ui_self_test(void)
{
    struct ui_layout layout;
    enum ui_element_id hit;
    enum ui_status status;

    self_test_failure = "First Light UI self-test passed";
    if (!ui_font_self_test()) {
        self_test_failure = "UI font suite rejected its valid fixture";
        return false;
    }
    status = ui_layout_build(800U, 600U, &layout);
    if (status == UI_STATUS_OK) {
        status = ui_layout_build(1024U, 768U, &layout);
    }
    if (status == UI_STATUS_OK) {
        status = ui_layout_build(1280U, 720U, &layout);
    }
    if (status != UI_STATUS_OK) {
        self_test_failure = ui_status_string(status);
        return false;
    }
    if (ui_layout_build(799U, 600U, &layout) !=
            UI_STATUS_UNSUPPORTED_GEOMETRY) {
        self_test_failure = "UI layout geometry acceptance is inconsistent";
        return false;
    }
    if (ui_layout_build(1280U, 720U, &layout) != UI_STATUS_OK) {
        self_test_failure = "UI layout valid fixture could not be restored";
        return false;
    }

    struct ui_layout damaged = layout;
    damaged.dock_items[1].id = damaged.dock_items[0].id;
    if (ui_layout_validate(&damaged) != UI_STATUS_DUPLICATE_ELEMENT_ID) {
        self_test_failure = "duplicate dock element ID was accepted";
        return false;
    }
    damaged = layout;
    damaged.dock_items[1].bounds = damaged.dock_items[0].bounds;
    if (ui_layout_validate(&damaged) != UI_STATUS_DOCK_OVERLAP) {
        self_test_failure = "overlapping dock elements were accepted";
        return false;
    }
    damaged = layout;
    damaged.logo.x = UINT32_MAX - 3U;
    damaged.logo.width = 8U;
    if (ui_layout_validate(&damaged) != UI_STATUS_RECTANGLE_OVERFLOW) {
        self_test_failure = "overflowing UI rectangle was accepted";
        return false;
    }

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const struct ui_rect bounds = layout.dock_items[index].bounds;
        const struct ui_point inside = {
            (int32_t)bounds.x, (int32_t)bounds.y
        };
        const struct ui_point right_edge = {
            (int32_t)(bounds.x + bounds.width), (int32_t)bounds.y
        };
        const struct ui_point bottom_edge = {
            (int32_t)bounds.x, (int32_t)(bounds.y + bounds.height)
        };

        if (ui_hit_test(&layout, inside, &hit) != UI_STATUS_OK ||
            hit != layout.dock_items[index].id ||
            ui_hit_test(&layout, right_edge, &hit) != UI_STATUS_OK ||
            hit == layout.dock_items[index].id ||
            ui_hit_test(&layout, bottom_edge, &hit) != UI_STATUS_OK ||
            hit == layout.dock_items[index].id) {
            self_test_failure = "half-open dock hit-test edge is inconsistent";
            return false;
        }
    }

    const struct ui_rect first = { 0U, 0U, UI_CURSOR_WIDTH, UI_CURSOR_HEIGHT };
    const struct ui_rect last = { 100U, 100U, UI_CURSOR_WIDTH, UI_CURSOR_HEIGHT };
    const struct ui_rect both = rect_union(first, last);
    if (both.x != 0U || both.y != 0U || both.width != 112U ||
        both.height != 118U || UI_CURSOR_HOTSPOT_X >= UI_CURSOR_WIDTH ||
        UI_CURSOR_HOTSPOT_Y >= UI_CURSOR_HEIGHT) {
        self_test_failure = "cursor damage union or hotspot is invalid";
        return false;
    }
    if (next_focus(UI_ELEMENT_DOCK_TERMINAL, true) !=
            UI_ELEMENT_DOCK_ABOUT ||
        next_focus(UI_ELEMENT_DOCK_ABOUT, false) !=
            UI_ELEMENT_DOCK_TERMINAL) {
        self_test_failure = "keyboard focus wrap is invalid";
        return false;
    }
    if (!event_queue_self_test()) {
        self_test_failure = event_queue_failure;
        return false;
    }

    const uint64_t stable = synthetic_render_hash(false);
    if (stable != UINT64_C(0xB115EB0C0BE1D0E1) ||
        stable == synthetic_render_hash(true)) {
        self_test_failure = "synthetic First Light render hash is invalid";
        return false;
    }
    return true;
}

const char *ui_self_test_failure(void)
{
    return self_test_failure;
}

enum ui_status ui_verify_installed(struct ui_proof *proof)
{
    if (proof == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (!state.active || canvas == NULL || !ui_font_is_verified() ||
        ui_layout_validate(&state.layout) != UI_STATUS_OK ||
        state.focus <= UI_ELEMENT_NONE || state.focus >= UI_ELEMENT_COUNT ||
        state.hover >= UI_ELEMENT_COUNT || state.pressed >= UI_ELEMENT_COUNT ||
        state.active_panel >= UI_PANEL_COUNT) {
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    if (state.pointer_present) {
        const struct pointer_state pointer = pointer_get_state();

        if (!pointer.present || pointer.bound_width != canvas->width ||
            pointer.bound_height != canvas->height || state.pointer.x < 0 ||
            state.pointer.y < 0 || (uint32_t)state.pointer.x >= canvas->width ||
            (uint32_t)state.pointer.y >= canvas->height) {
            return UI_STATUS_INSTALLED_PROOF_FAILURE;
        }
    }
    if (state.theme.white != framebuffer_pack(0xF7U, 0xF6U, 0xF0U) ||
        state.theme.ink != framebuffer_pack(0x10U, 0x10U, 0x12U) ||
        state.theme.desktop_dark != framebuffer_pack(0x59U, 0x59U, 0x76U) ||
        state.theme.desktop_light != framebuffer_pack(0x66U, 0x66U, 0x84U) ||
        state.theme.title_active != framebuffer_pack(0x18U, 0x18U, 0x1CU) ||
        state.theme.title_inactive != framebuffer_pack(0x7AU, 0x7AU, 0x82U) ||
        state.theme.accent_teal != framebuffer_pack(0x4FU, 0x83U, 0x7FU) ||
        state.theme.accent_gold != framebuffer_pack(0xC4U, 0xA4U, 0x4EU) ||
        state.theme.accent_green != framebuffer_pack(0x59U, 0x85U, 0x61U) ||
        state.theme.accent_red != framebuffer_pack(0xA5U, 0x50U, 0x50U) ||
        state.theme.accent_violet != framebuffer_pack(0x70U, 0x59U, 0x84U) ||
        state.theme.shadow != framebuffer_pack(0x35U, 0x35U, 0x42U) ||
        state.theme.window_face != framebuffer_pack(0xD7U, 0xD6U, 0xCEU)) {
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        if (state.layout.dock_items[index].id !=
                (enum ui_element_id)(index + 1U) ||
            state.layout.dock_items[index].action !=
                (enum ui_action)(index + 1U) ||
            state.layout.dock_items[index].panel !=
                (enum ui_panel_id)(index + 1U)) {
            return UI_STATUS_INSTALLED_PROOF_FAILURE;
        }
    }

    const uint64_t first_hash = surface_hash();
    if (render_region(state.layout.surface, true) != UI_STATUS_OK) {
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    const uint64_t second_hash = surface_hash();
    if (first_hash != second_hash) {
        return UI_STATUS_INSTALLED_PROOF_FAILURE;
    }
    state.stable_render_hash = second_hash;

    const struct boot_ledger *ledger = boot_ledger_installed();
    *proof = (struct ui_proof){
        .width = canvas->width,
        .height = canvas->height,
        .dock_items = UI_DOCK_ITEM_COUNT,
        .events = state.events.drained,
        .panels = state.renders.panel_transitions,
        .cursor_moves = state.renders.cursor_moves,
        .damage_rectangles = state.renders.damage_rectangles,
        .glyphs = state.renders.glyphs,
        .ledger_fingerprint = ledger == NULL ? 0U : ledger->fingerprint,
        .render_hash = second_hash
    };
    return UI_STATUS_OK;
}

const char *ui_panel_name(enum ui_panel_id panel)
{
    static const char *const names[] = {
        "None", "Terminal", "Ledger", "System", "About"
    };

    if ((size_t)panel >= sizeof(names) / sizeof(names[0])) {
        return "Unknown panel";
    }
    return names[panel];
}

const char *ui_element_name(enum ui_element_id element)
{
    static const char *const names[] = {
        "none", "Terminal", "Ledger", "System", "About"
    };

    if ((size_t)element >= sizeof(names) / sizeof(names[0])) {
        return "unknown element";
    }
    return names[element];
}

const char *ui_status_string(enum ui_status status)
{
    static const char *const messages[] = {
        "ok",
        "null UI argument",
        "First Light is already initialized",
        "First Light is not initialized",
        "First Light is not active",
        "unsupported First Light framebuffer geometry",
        "UI rectangle arithmetic overflowed",
        "UI rectangle lies outside its surface",
        "duplicate UI element identifier",
        "dock item rectangles overlap",
        "panel client rectangle is empty",
        "UI text baseline lies outside its box",
        "software cursor hotspot is invalid",
        "UI hit test selected more than one element",
        "UI event queue is full",
        "UI event type is invalid",
        "UI element is invalid",
        "UI panel is invalid",
        "UI font rendering failed",
        "cached surface rendering failed",
        "canonical logo rendering failed",
        "terminal viewport rendering failed",
        "installed First Light proof failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        (size_t)UI_STATUS_INSTALLED_PROOF_FAILURE + 1U,
        "UI status messages are out of sync");
    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown UI status";
    }
    return messages[status];
}
