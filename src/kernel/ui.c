/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/boot_ledger.h>
#include <sapote/cpu.h>
#include <sapote/framebuffer.h>
#include <sapote/fat32_fs.h>
#include <sapote/heap.h>
#include <sapote/logo.h>
#include <sapote/memory.h>
#include <sapote/pci.h>
#include <sapote/pointer.h>
#include <sapote/screen.h>
#include <sapote/thread.h>
#include <sapote/ui.h>
#include <sapote/ui_font.h>
#include <sapote/wallpaper.h>

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
#define UI_DOCK_WIDTH 444U
#define UI_DOCK_ITEM_WIDTH 104U
#define UI_DOCK_ITEM_HEIGHT 116U
#define UI_DOCK_GAP 14U
#define UI_DOCK_PADDING 52U
#define UI_DOCK_HEIGHT 126U
#define UI_FONT_ASCENT 12U
#define UI_FONT_DESCENT 4U
#define UI_FONT_ADVANCE 8U
#define UI_PANEL_TITLE_HEIGHT 26U

static const char label_files[] = "Files";
static const char label_terminal[] = "Terminal";
static const char label_notes[] = "Notes";

static struct ui_state state;
static struct surface *canvas;
static struct ui_event event_queue[UI_EVENT_QUEUE_CAPACITY];
static size_t event_count;
static uint32_t logo_pixels[UI_LOGO_PIXELS];
static uint32_t wallpaper_pixels[1024U * 768U];
static struct sapfs_list_entry file_entries[12U];
static size_t file_entry_count;
static char file_directory[SAPFS_MAX_PATH + 1U] = ".";
static char note_path[SAPFS_MAX_PATH + 1U] = "NOTES.TXT";
static char note_buffer[1536U];
static size_t note_length;
static bool note_dirty;
static bool terminal_welcomed;
static char app_status[80U] = "data volume ready";
static const char *self_test_failure = "First Environment UI self-test not run";
static const char *event_queue_failure = "UI event queue self-test not run";

static const uint32_t cursor_outer[UI_CURSOR_HEIGHT] = {
    UINT32_C(0xE0000000), UINT32_C(0xF0000000), UINT32_C(0xFC000000),
    UINT32_C(0xFE000000), UINT32_C(0xFF000000), UINT32_C(0xFF800000),
    UINT32_C(0xFFE00000), UINT32_C(0xFFF00000), UINT32_C(0xFFF80000),
    UINT32_C(0xFFFE0000), UINT32_C(0xFFFF0000), UINT32_C(0xFFFF8000),
    UINT32_C(0xFFFFE000), UINT32_C(0xFFFFF000), UINT32_C(0xFFFFF800),
    UINT32_C(0xFFFFFC00), UINT32_C(0xFFFFFF00), UINT32_C(0xFFFFFF80),
    UINT32_C(0xFFFFFFC0), UINT32_C(0xFFFFFFC0), UINT32_C(0xFFFFFFC0),
    UINT32_C(0xFFFE0000), UINT32_C(0xFFFF0000), UINT32_C(0xFFFF0000),
    UINT32_C(0xFFFF8000), UINT32_C(0xFDFF8000), UINT32_C(0xF9FFC000),
    UINT32_C(0xF0FFE000), UINT32_C(0xE0FFE000), UINT32_C(0xC07FF000),
    UINT32_C(0x007FF000), UINT32_C(0x003FF800), UINT32_C(0x001FF800),
    UINT32_C(0x001FF800), UINT32_C(0x000FF000), UINT32_C(0x000FC000),
    UINT32_C(0x00070000)
};
static const uint32_t cursor_inner[UI_CURSOR_HEIGHT] = {
    UINT32_C(0x80000000), UINT32_C(0xC0000000), UINT32_C(0xE0000000),
    UINT32_C(0xF8000000), UINT32_C(0xFC000000), UINT32_C(0xFE000000),
    UINT32_C(0xFF000000), UINT32_C(0xFFC00000), UINT32_C(0xFFE00000),
    UINT32_C(0xFFF00000), UINT32_C(0xFFFC0000), UINT32_C(0xFFFE0000),
    UINT32_C(0xFFFF0000), UINT32_C(0xFFFFC000), UINT32_C(0xFFFFE000),
    UINT32_C(0xFFFFF000), UINT32_C(0xFFFFF800), UINT32_C(0xFFFFFE00),
    UINT32_C(0xFFFFFF00), UINT32_C(0xFFFFFF80), UINT32_C(0xFFF80000),
    UINT32_C(0xFFFC0000), UINT32_C(0xFFFC0000), UINT32_C(0xFDFE0000),
    UINT32_C(0xF8FE0000), UINT32_C(0xF0FF0000), UINT32_C(0xE07F0000),
    UINT32_C(0xC07F8000), UINT32_C(0x803FC000), UINT32_C(0x003FC000),
    UINT32_C(0x001FE000), UINT32_C(0x000FE000), UINT32_C(0x000FF000),
    UINT32_C(0x0007E000), UINT32_C(0x00078000), UINT32_C(0x00020000),
    UINT32_C(0x00000000)
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
    if (layout == NULL || element < UI_ELEMENT_DOCK_FILES ||
        element > UI_ELEMENT_DOCK_NOTES) {
        return (struct ui_rect){ 0U, 0U, 0U, 0U };
    }
    const struct ui_rect item =
        layout->dock_items[(size_t)element - 1U].bounds;
    const uint32_t top = item.y >= 26U ? item.y - 26U : 0U;

    return (struct ui_rect){ item.x, top, item.width,
        item.height + item.y - top + 16U };
}

static enum ui_panel_id panel_for_element(enum ui_element_id element)
{
    if (element < UI_ELEMENT_DOCK_FILES ||
        element > UI_ELEMENT_DOCK_NOTES) {
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
    uint32_t panel_width;
    uint32_t panel_height;

    if (layout == NULL) {
        return UI_STATUS_NULL_ARGUMENT;
    }
    if (width < UI_MIN_WIDTH || height < UI_MIN_HEIGHT ||
        width > UI_MAX_WIDTH || height > UI_MAX_HEIGHT) {
        return UI_STATUS_UNSUPPORTED_GEOMETRY;
    }
    *layout = (struct ui_layout){ 0 };
    layout->surface = (struct ui_rect){ 0U, 0U, width, height };
    layout->menu_bar = (struct ui_rect){ 0U, 0U, width, UI_MENU_HEIGHT };
    layout->workspace_bar = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->menu_baseline = 17U;
    layout->hero_window = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->logo = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->wordmark = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->motto = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->version_label = (struct ui_rect){ 0U, UI_MENU_HEIGHT, 1U, 1U };
    layout->dock = (struct ui_rect){
        (width - UI_DOCK_WIDTH) / 2U, height - UI_DOCK_HEIGHT,
        UI_DOCK_WIDTH, UI_DOCK_HEIGHT
    };

    static const enum ui_element_id ids[UI_DOCK_ITEM_COUNT] = {
        UI_ELEMENT_DOCK_FILES, UI_ELEMENT_DOCK_TERMINAL,
        UI_ELEMENT_DOCK_NOTES
    };
    static const char *const labels[UI_DOCK_ITEM_COUNT] = {
        label_files, label_terminal, label_notes
    };
    static const enum ui_action actions[UI_DOCK_ITEM_COUNT] = {
        UI_ACTION_OPEN_FILES, UI_ACTION_OPEN_TERMINAL, UI_ACTION_OPEN_NOTES
    };
    static const enum ui_panel_id panels[UI_DOCK_ITEM_COUNT] = {
        UI_PANEL_FILES, UI_PANEL_TERMINAL, UI_PANEL_NOTES
    };

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const uint32_t x = layout->dock.x + UI_DOCK_PADDING +
            (uint32_t)index * (UI_DOCK_ITEM_WIDTH + UI_DOCK_GAP);

        layout->dock_items[index] = (struct ui_dock_item){
            .id = ids[index],
            .label = labels[index],
            .bounds = { x, layout->dock.y + 1U,
                UI_DOCK_ITEM_WIDTH, UI_DOCK_ITEM_HEIGHT },
            .icon_bounds = { x + 12U, layout->dock.y + 2U, 80U, 80U },
            .action = actions[index],
            .panel = panels[index]
        };
    }
    layout->dock_label_baseline = layout->dock.y + 18U;

    panel_width = width - 80U < 860U ? width - 80U : 860U;
    panel_height = layout->dock.y - 56U;
    layout->panel = (struct ui_rect){
        (width - panel_width) / 2U, 40U,
        panel_width, panel_height
    };
    layout->panel_client = (struct ui_rect){
        layout->panel.x + 10U, layout->panel.y + 38U,
        panel_width - 20U, panel_height - 48U
    };
    layout->panel_title_baseline = layout->panel.y + 22U;
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
        layout->surface, layout->menu_bar, layout->dock, layout->panel,
        layout->panel_client,
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
    for (enum ui_element_id id = UI_ELEMENT_DOCK_FILES;
         id <= UI_ELEMENT_DOCK_NOTES; id = (enum ui_element_id)(id + 1)) {
        if (!seen[id]) {
            return UI_STATUS_BAD_ELEMENT;
        }
    }

    if (!baseline_fits(layout->menu_bar, layout->menu_baseline) ||
        !baseline_fits(layout->panel, layout->panel_title_baseline) ||
        !baseline_fits(layout->panel_client, layout->panel_text_baseline)) {
        return UI_STATUS_TEXT_BASELINE_OUT_OF_BOUNDS;
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
    theme->white = framebuffer_pack(0xF8U, 0xFAU, 0xF8U);
    theme->ink = framebuffer_pack(0x18U, 0x21U, 0x24U);
    theme->desktop_dark = framebuffer_pack(0x07U, 0x16U, 0x22U);
    theme->desktop_light = framebuffer_pack(0x1CU, 0x4BU, 0x5AU);
    theme->title_active = framebuffer_pack(0x1CU, 0x29U, 0x2DU);
    theme->title_inactive = framebuffer_pack(0x91U, 0x9DU, 0xA2U);
    theme->accent_teal = framebuffer_pack(0x68U, 0xA9U, 0xC5U);
    theme->accent_gold = framebuffer_pack(0xE6U, 0xC4U, 0x62U);
    theme->accent_green = framebuffer_pack(0x8EU, 0xADU, 0x89U);
    theme->accent_red = framebuffer_pack(0xD9U, 0x55U, 0x4FU);
    theme->accent_violet = framebuffer_pack(0x94U, 0x7BU, 0xB4U);
    theme->shadow = framebuffer_pack(0x05U, 0x0CU, 0x12U);
    theme->window_face = framebuffer_pack(0xD9U, 0xDFU, 0xE0U);
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

static enum ui_status draw_wallpaper(struct ui_rect damage)
{
    const struct ui_rect clipped = rect_intersection(state.layout.surface,
        damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        const uint32_t target_y = clipped.y + y;
        const uint32_t source_y = target_y * 768U /
            state.layout.surface.height;

        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t target_x = clipped.x + x;
            const uint32_t source_x = target_x * 1024U /
                state.layout.surface.width;

            if (surface_pixel(canvas, target_x, target_y,
                    wallpaper_pixels[source_y * 1024U + source_x]) !=
                SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static uint8_t packed_channel(uint32_t pixel, uint8_t shift)
{
    return (uint8_t)((pixel >> shift) & 0xFFU);
}

static uint32_t blend_packed(uint32_t under, uint32_t over, uint8_t alpha)
{
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    const uint32_t inverse = 255U - alpha;
    const uint8_t red = (uint8_t)((
        (uint32_t)packed_channel(over, framebuffer.red_position) * alpha +
        (uint32_t)packed_channel(under, framebuffer.red_position) * inverse +
        127U) / 255U);
    const uint8_t green = (uint8_t)((
        (uint32_t)packed_channel(over, framebuffer.green_position) * alpha +
        (uint32_t)packed_channel(under, framebuffer.green_position) * inverse +
        127U) / 255U);
    const uint8_t blue = (uint8_t)((
        (uint32_t)packed_channel(over, framebuffer.blue_position) * alpha +
        (uint32_t)packed_channel(under, framebuffer.blue_position) * inverse +
        127U) / 255U);

    return framebuffer_pack(red, green, blue);
}

static enum ui_status translucent_fill(
    struct ui_rect rectangle,
    struct ui_rect damage,
    uint32_t over,
    uint8_t alpha
)
{
    const struct ui_rect clipped = rect_intersection(rectangle, damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            uint32_t under;

            if (surface_read_pixel(canvas, clipped.x + x, clipped.y + y,
                    &under) != SURFACE_STATUS_OK ||
                surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    blend_packed(under, over, alpha)) != SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
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

static enum ui_status draw_logo_color(
    struct ui_rect bounds,
    struct ui_rect damage
)
{
    const struct ui_rect clipped = rect_intersection(bounds, damage);

    for (uint32_t y = 0U; y < clipped.height; ++y) {
        for (uint32_t x = 0U; x < clipped.width; ++x) {
            const uint32_t source_x =
                (clipped.x - bounds.x + x) * UI_LOGO_WIDTH / bounds.width;
            const uint32_t source_y =
                (clipped.y - bounds.y + y) * UI_LOGO_HEIGHT / bounds.height;

            if (surface_pixel(canvas, clipped.x + x, clipped.y + y,
                    logo_pixels[source_y * UI_LOGO_WIDTH + source_x]) !=
                SURFACE_STATUS_OK) {
                return UI_STATUS_SURFACE_FAILURE;
            }
        }
    }
    return UI_STATUS_OK;
}

static enum ui_status gradient_rect(
    struct ui_rect bounds,
    struct ui_rect damage,
    uint8_t top_red,
    uint8_t top_green,
    uint8_t top_blue,
    uint8_t bottom_red,
    uint8_t bottom_green,
    uint8_t bottom_blue
)
{
    enum ui_status status = UI_STATUS_OK;
    const uint32_t denominator = bounds.height > 1U ? bounds.height - 1U : 1U;

    for (uint32_t row = 0U; row < bounds.height && status == UI_STATUS_OK;
         ++row) {
        const uint32_t inverse = denominator - row;
        const uint8_t red = (uint8_t)(((uint32_t)top_red * inverse +
            (uint32_t)bottom_red * row) / denominator);
        const uint8_t green = (uint8_t)(((uint32_t)top_green * inverse +
            (uint32_t)bottom_green * row) / denominator);
        const uint8_t blue = (uint8_t)(((uint32_t)top_blue * inverse +
            (uint32_t)bottom_blue * row) / denominator);

        status = fill_clipped((struct ui_rect){ bounds.x, bounds.y + row,
            bounds.width, 1U }, damage, framebuffer_pack(red, green, blue));
    }
    return status;
}

static enum ui_status draw_icon(
    enum ui_element_id id,
    struct ui_rect bounds,
    struct ui_rect damage,
    uint32_t pixel
)
{
    (void)pixel;
    enum ui_status status = translucent_fill((struct ui_rect){
        bounds.x + 4U, bounds.y + 5U, bounds.width - 4U,
        bounds.height - 3U }, damage, state.theme.shadow, 110U);

    if (id == UI_ELEMENT_DOCK_FILES) {
        const struct ui_rect tab = { bounds.x + 5U, bounds.y + 7U,
            bounds.width * 5U / 11U, bounds.height / 3U };
        const struct ui_rect folder = { bounds.x + 3U, bounds.y + 16U,
            bounds.width - 6U, bounds.height - 20U };
        const struct ui_rect inset = { bounds.x + bounds.width / 4U,
            bounds.y + bounds.height / 3U, bounds.width / 2U,
            bounds.height * 5U / 12U };

        if (status == UI_STATUS_OK) {
            status = gradient_rect(tab, damage, 0xC7U, 0xE5U, 0xF5U,
                0x6BU, 0xA9U, 0xCBU);
        }
        if (status == UI_STATUS_OK) {
            status = gradient_rect(folder, damage, 0xB6U, 0xDCU, 0xF0U,
                0x4FU, 0x8CU, 0xB4U);
        }
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(folder, damage, 2U,
                framebuffer_pack(0x3CU, 0x65U, 0x79U));
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped(inset, damage,
                framebuffer_pack(0x2EU, 0x3BU, 0x40U));
        }
        if (status == UI_STATUS_OK) {
            status = draw_logo_color((struct ui_rect){ inset.x + 6U,
                inset.y + 3U, inset.width - 12U, inset.height - 6U },
                damage);
        }
    } else if (id == UI_ELEMENT_DOCK_TERMINAL) {
        const struct ui_rect shell = { bounds.x + 2U, bounds.y + 2U,
            bounds.width - 4U, bounds.height - 5U };
        const struct ui_rect screen = { shell.x + 7U, shell.y + 8U,
            shell.width - 14U, shell.height - 19U };

        if (status == UI_STATUS_OK) {
            status = gradient_rect(shell, damage, 0xF6U, 0xF9U, 0xF9U,
                0x7DU, 0x8CU, 0x91U);
        }
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(shell, damage, 2U,
                framebuffer_pack(0x51U, 0x5EU, 0x62U));
        }
        if (status == UI_STATUS_OK) {
            status = gradient_rect(screen, damage, 0x43U, 0x4AU, 0x4CU,
                0x06U, 0x09U, 0x0AU);
        }
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(screen, damage, 2U, state.theme.ink);
        }
        for (uint32_t step = 0U; step < 3U && status == UI_STATUS_OK;
             ++step) {
            status = fill_clipped((struct ui_rect){ screen.x + 10U + step * 3U,
                screen.y + 12U + step * 3U, 5U, 3U }, damage,
                state.theme.white);
            if (status == UI_STATUS_OK) {
                status = fill_clipped((struct ui_rect){
                    screen.x + 10U + step * 3U,
                    screen.y + 24U - step * 3U, 5U, 3U }, damage,
                    state.theme.white);
            }
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ screen.x + 29U,
                screen.y + 28U, 18U, 3U }, damage, state.theme.white);
        }
    } else if (id == UI_ELEMENT_DOCK_NOTES) {
        const struct ui_rect page = { bounds.x + 7U, bounds.y + 4U,
            bounds.width - 14U, bounds.height - 8U };

        if (status == UI_STATUS_OK) {
            status = gradient_rect(page, damage, 0xFFU, 0xFFU, 0xF8U,
                0xE4U, 0xE2U, 0xD6U);
        }
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(page, damage, 1U,
                framebuffer_pack(0xA0U, 0xA0U, 0x98U));
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ page.x, page.y,
                page.width, 13U }, damage,
                framebuffer_pack(0x72U, 0x75U, 0x76U));
        }
        if (status == UI_STATUS_OK) {
            status = fill_clipped((struct ui_rect){ page.x + 13U,
                page.y + 15U, 2U, page.height - 18U }, damage,
                framebuffer_pack(0xE9U, 0x78U, 0x78U));
        }
        for (uint32_t row = page.y + 22U;
             row + 1U < page.y + page.height && status == UI_STATUS_OK;
             row += 8U) {
            status = fill_clipped((struct ui_rect){ page.x + 4U, row,
                page.width - 8U, 1U }, damage,
                framebuffer_pack(0xB8U, 0xD1U, 0xDCU));
        }
    } else {
        status = UI_STATUS_BAD_ELEMENT;
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
        status = gradient_rect(title, damage, 0xF4U, 0xF7U, 0xF7U,
            0x99U, 0xA5U, 0xAAU);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            title.x, title.y + title.height - 1U, title.width, 1U
        }, damage, state.theme.ink);
    }
    if (close_box) {
        const struct ui_rect close = {
            title.x + 6U, title.y + 4U, 18U, 18U
        };

        status = gradient_rect(close, damage, 0xF6U, 0x8AU, 0x82U,
            0xBCU, 0x2FU, 0x2AU);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(close, damage, 1U, state.theme.ink);
        }
        for (uint32_t step = 0U; step < 8U && status == UI_STATUS_OK;
             ++step) {
            status = fill_clipped((struct ui_rect){ close.x + 5U + step,
                close.y + 5U + step, 2U, 2U }, damage, state.theme.white);
            if (status == UI_STATUS_OK) {
                status = fill_clipped((struct ui_rect){ close.x + 12U - step,
                    close.y + 5U + step, 2U, 2U }, damage,
                    state.theme.white);
            }
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
    const bool adjacent = state.hover >= UI_ELEMENT_DOCK_FILES &&
        state.hover <= UI_ELEMENT_DOCK_NOTES && !hovered &&
        (state.hover + 1 == item->id || item->id + 1 == state.hover);
    struct ui_rect icon = item->icon_bounds;

    if (hovered) {
        icon.x -= 8U;
        icon.y -= 13U;
        icon.width += 16U;
        icon.height += 16U;
    } else if (adjacent) {
        icon.x -= 3U;
        icon.y -= 5U;
        icon.width += 6U;
        icon.height += 6U;
    } else if (pressed) {
        icon.y += 3U;
    }

    enum ui_status status = draw_icon(item->id, icon, damage,
        state.theme.ink);
    if (status == UI_STATUS_OK) {
        const uint32_t reflection_height = 18U;

        for (uint32_t row = 0U; row < reflection_height &&
             status == UI_STATUS_OK; ++row) {
            const uint32_t source_y = icon.y + icon.height - 1U - row * 2U;
            const uint32_t destination_y = icon.y + icon.height + 1U + row;
            const uint8_t alpha = (uint8_t)(72U - row * 4U);

            if (destination_y >= state.layout.surface.height) {
                break;
            }
            for (uint32_t x = 0U; x < icon.width; ++x) {
                uint32_t source;
                uint32_t under;

                if (!rect_contains_point(damage, (struct ui_point){
                        (int32_t)(icon.x + x), (int32_t)destination_y })) {
                    continue;
                }
                if (surface_read_pixel(canvas, icon.x + x, source_y,
                        &source) != SURFACE_STATUS_OK ||
                    surface_read_pixel(canvas, icon.x + x, destination_y,
                        &under) != SURFACE_STATUS_OK ||
                    surface_pixel(canvas, icon.x + x, destination_y,
                        blend_packed(under, source, alpha)) !=
                        SURFACE_STATUS_OK) {
                    status = UI_STATUS_SURFACE_FAILURE;
                    break;
                }
            }
        }
    }
    if (status == UI_STATUS_OK && active) {
        status = fill_clipped((struct ui_rect){
            item->bounds.x + item->bounds.width / 2U - 5U,
            item->bounds.y + 110U, 10U, 4U }, damage, state.theme.white);
    }
    if (status == UI_STATUS_OK &&
        (hovered || (focused && !state.pointer_present))) {
        const struct ui_rect bubble = { item->bounds.x + 8U,
            item->bounds.y - 24U, item->bounds.width - 16U, 21U };

        status = translucent_fill(bubble, damage, state.theme.shadow, 210U);
        if (status == UI_STATUS_OK) {
            status = draw_text(bubble, damage,
                centered_text_x(bubble, item->label), bubble.y + 15U,
                item->label, state.theme.white);
        }
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

static bool strings_equal(const char *left, const char *right)
{
    size_t index = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (left[index] != '\0' && left[index] == right[index]) {
        ++index;
    }
    return left[index] == right[index];
}

static bool copy_string(char *destination, size_t capacity, const char *source)
{
    size_t index = 0U;

    if (destination == NULL || source == NULL || capacity == 0U) {
        return false;
    }
    while (source[index] != '\0') {
        if (index + 1U >= capacity) {
            destination[0] = '\0';
            return false;
        }
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return true;
}

static void set_app_status(const char *prefix, enum sapfs_status status)
{
    size_t at = append_text(app_status, sizeof(app_status), 0U, prefix);

    if (status != SAPFS_STATUS_OK) {
        at = append_text(app_status, sizeof(app_status), at, ": ");
        (void)append_text(app_status, sizeof(app_status), at,
            sapfs_status_string(status));
    }
}

static bool entry_path(const char *name, char *path)
{
    size_t at = 0U;

    if (!strings_equal(file_directory, ".")) {
        at = append_text(path, SAPFS_MAX_PATH + 1U, at, file_directory);
        at = append_text(path, SAPFS_MAX_PATH + 1U, at, "/");
    }
    at = append_text(path, SAPFS_MAX_PATH + 1U, at, name);
    return at != 0U && at <= SAPFS_MAX_PATH;
}

static enum sapfs_status files_refresh(void)
{
    file_entry_count = 0U;
    const enum sapfs_status status = sapfs_list(SAPFS_VOLUME_DATA,
        file_directory, file_entries,
        sizeof(file_entries) / sizeof(file_entries[0]), &file_entry_count);

    set_app_status(status == SAPFS_STATUS_OK ?
        "data volume / fat32 / synchronized view" : "Files", status);
    return status;
}

static void files_up(void)
{
    size_t length = 0U;
    size_t slash = SIZE_MAX;

    if (strings_equal(file_directory, ".")) {
        set_app_status("already at data root", SAPFS_STATUS_OK);
        return;
    }
    while (file_directory[length] != '\0') {
        if (file_directory[length] == '/') {
            slash = length;
        }
        ++length;
    }
    if (slash == SIZE_MAX) {
        (void)copy_string(file_directory, sizeof(file_directory), ".");
    } else {
        file_directory[slash] = '\0';
    }
    (void)files_refresh();
}

static void files_create(bool directory)
{
    char name[13U];
    char path[SAPFS_MAX_PATH + 1U];
    enum sapfs_status status = SAPFS_STATUS_FULL;

    for (uint32_t number = 1U; number <= 9U; ++number) {
        size_t at = append_text(name, sizeof(name), 0U,
            directory ? "FOLDER" : "NEW");

        name[at++] = (char)('0' + number);
        name[at] = '\0';
        if (!directory) {
            (void)append_text(name, sizeof(name), at, ".TXT");
        }
        if (!entry_path(name, path)) {
            status = SAPFS_STATUS_PATH;
            break;
        }
        struct sapfs_stat stat;
        status = sapfs_stat_path(SAPFS_VOLUME_DATA, path, &stat);
        if (status != SAPFS_STATUS_NOT_FOUND) {
            continue;
        }
        status = directory ? sapfs_mkdir(SAPFS_VOLUME_DATA, path) :
            sapfs_create(SAPFS_VOLUME_DATA, path);
        if (status == SAPFS_STATUS_OK) {
            set_app_status(directory ? "created folder" : "created file",
                SAPFS_STATUS_OK);
            (void)files_refresh();
            return;
        }
        break;
    }
    set_app_status(directory ? "new folder" : "new file", status);
}

static enum sapfs_status note_load(void)
{
    sapfs_handle handle;
    size_t read_bytes = 0U;
    enum sapfs_status status = sapfs_open(SAPFS_VOLUME_DATA, note_path,
        SAPFS_ACCESS_READ, &handle);

    note_length = 0U;
    note_buffer[0] = '\0';
    note_dirty = false;
    if (status == SAPFS_STATUS_NOT_FOUND) {
        set_app_status("new note / Ctrl+S to save", SAPFS_STATUS_OK);
        return SAPFS_STATUS_OK;
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_read(handle, (uint8_t *)note_buffer,
            sizeof(note_buffer) - 1U, &read_bytes);
        const enum sapfs_status close_status = sapfs_close(handle);

        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == SAPFS_STATUS_OK) {
        note_length = read_bytes;
        note_buffer[note_length] = '\0';
        set_app_status("note loaded from data volume", SAPFS_STATUS_OK);
    } else {
        set_app_status("open note", status);
    }
    return status;
}

static enum sapfs_status note_save(void)
{
    struct sapfs_stat stat;
    sapfs_handle handle;
    size_t written = 0U;
    enum sapfs_status status = sapfs_stat_path(SAPFS_VOLUME_DATA,
        note_path, &stat);

    if (status == SAPFS_STATUS_NOT_FOUND) {
        status = sapfs_create(SAPFS_VOLUME_DATA, note_path);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_truncate(SAPFS_VOLUME_DATA, note_path, 0U);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_open(SAPFS_VOLUME_DATA, note_path,
            SAPFS_ACCESS_WRITE, &handle);
    }
    if (status == SAPFS_STATUS_OK) {
        status = sapfs_write(handle, (const uint8_t *)note_buffer,
            note_length, &written);
        const enum sapfs_status close_status = sapfs_close(handle);

        if (status == SAPFS_STATUS_OK && close_status != SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == SAPFS_STATUS_OK && written == note_length) {
        status = sapfs_sync(SAPFS_VOLUME_DATA);
    }
    if (status == SAPFS_STATUS_OK && written == note_length) {
        note_dirty = false;
        set_app_status("saved and synchronized to fat32", SAPFS_STATUS_OK);
    } else {
        set_app_status("save note", status == SAPFS_STATUS_OK ?
            SAPFS_STATUS_WRITEBACK : status);
    }
    return status;
}

static struct ui_rect file_entry_rect(size_t index)
{
    const struct ui_rect client = state.layout.panel_client;
    const uint32_t content_x = client.x + 190U;
    const uint32_t content_width = client.width - 190U;
    const uint32_t cell_width = content_width / 4U;

    return (struct ui_rect){ content_x + (uint32_t)(index % 4U) * cell_width,
        client.y + 54U + (uint32_t)(index / 4U) * 112U,
        cell_width, 104U };
}

static enum ui_status draw_button(
    struct ui_rect button,
    struct ui_rect damage,
    const char *label
)
{
    enum ui_status status = gradient_rect(button, damage,
        0xF8U, 0xFAU, 0xFAU, 0xA8U, 0xB2U, 0xB5U);

    if (status == UI_STATUS_OK) {
        status = stroke_clipped(button, damage, 1U,
            framebuffer_pack(0x64U, 0x70U, 0x74U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(button, damage, centered_text_x(button, label),
            button.y + 18U, label, state.theme.ink);
    }
    return status;
}

static enum ui_status draw_files_app(struct ui_rect damage)
{
    const struct ui_rect client = state.layout.panel_client;
    const struct ui_rect toolbar = { client.x, client.y, client.width, 46U };
    const struct ui_rect sidebar = { client.x, client.y + 46U, 190U,
        client.height - 70U };
    const struct ui_rect content = { client.x + 190U, client.y + 46U,
        client.width - 190U, client.height - 70U };
    const struct ui_rect status_bar = { client.x,
        client.y + client.height - 24U, client.width, 24U };
    enum ui_status status = gradient_rect(toolbar, damage,
        0xF4U, 0xF5U, 0xF5U, 0xA9U, 0xADU, 0xAFU);

    if (status == UI_STATUS_OK) {
        status = fill_clipped(sidebar, damage,
            framebuffer_pack(0xD9U, 0xDDU, 0xDFU));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(content, damage, state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = gradient_rect(status_bar, damage,
            0xE8U, 0xEAU, 0xEAU, 0xB5U, 0xB8U, 0xBAU);
    }
    const struct ui_rect buttons[] = {
        { toolbar.x + 10U, toolbar.y + 8U, 56U, 26U },
        { toolbar.x + 76U, toolbar.y + 8U, 88U, 26U },
        { toolbar.x + 174U, toolbar.y + 8U, 104U, 26U },
        { toolbar.x + toolbar.width - 156U, toolbar.y + 8U, 68U, 26U },
        { toolbar.x + toolbar.width - 78U, toolbar.y + 8U, 68U, 26U }
    };
    static const char *const labels[] = {
        "Up", "New File", "New Folder", "Refresh", "Sync"
    };

    for (size_t index = 0U; index < 5U && status == UI_STATUS_OK; ++index) {
        status = draw_button(buttons[index], damage, labels[index]);
    }
    if (status == UI_STATUS_OK) {
        const struct ui_rect views[] = {
            { toolbar.x + 304U, toolbar.y + 8U, 34U, 26U },
            { toolbar.x + 338U, toolbar.y + 8U, 34U, 26U },
            { toolbar.x + 372U, toolbar.y + 8U, 34U, 26U }
        };
        static const char *const view_labels[] = { "[]", "=", "|||" };

        for (size_t index = 0U; index < 3U && status == UI_STATUS_OK;
             ++index) {
            status = draw_button(views[index], damage, view_labels[index]);
        }
    }
    if (status == UI_STATUS_OK) {
        const struct ui_rect location_field = { toolbar.x + 422U,
            toolbar.y + 8U, toolbar.width - 588U, 26U };
        char location[SAPFS_MAX_PATH + 16U];
        size_t at = append_text(location, sizeof(location), 0U, "data  / ");

        if (!strings_equal(file_directory, ".")) {
            (void)append_text(location, sizeof(location), at, file_directory);
        }
        status = fill_clipped(location_field, damage, state.theme.white);
        if (status == UI_STATUS_OK) {
            status = stroke_clipped(location_field, damage, 1U,
                framebuffer_pack(0x76U, 0x7CU, 0x80U));
        }
        if (status == UI_STATUS_OK) {
            status = draw_text(location_field, damage,
                location_field.x + 10U, location_field.y + 18U,
                location, state.theme.ink);
        }
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 12U,
            sidebar.y + 22U, "DEVICES", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = gradient_rect((struct ui_rect){ sidebar.x + 4U,
            sidebar.y + 29U, sidebar.width - 8U, 28U }, damage,
            0x70U, 0xA7U, 0xCCU, 0x2FU, 0x6FU, 0x9CU);
    }
    if (status == UI_STATUS_OK) {
        status = draw_logo_color((struct ui_rect){ sidebar.x + 14U,
            sidebar.y + 33U, 22U, 20U }, damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 46U,
            sidebar.y + 49U, "data", state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 12U,
            sidebar.y + 88U, "PLACES", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 116U, "Root", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 142U, "Current Folder", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 12U,
            sidebar.y + 184U, "VOLUME", state.theme.title_inactive);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 212U, "FAT32", state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(sidebar, damage, sidebar.x + 28U,
            sidebar.y + 238U, "Read / Write", state.theme.accent_green);
    }
    for (size_t index = 0U; index < file_entry_count &&
         index < 12U && status == UI_STATUS_OK; ++index) {
        const struct ui_rect tile = file_entry_rect(index);
        const struct sapfs_list_entry *entry = &file_entries[index];
        const struct ui_rect icon = { tile.x + (tile.width - 50U) / 2U,
            tile.y + 4U, 50U, 48U };

        if (entry->directory) {
            status = gradient_rect((struct ui_rect){ icon.x + 5U, icon.y,
                24U, 16U }, damage, 0xD3U, 0xE8U, 0xF2U,
                0x76U, 0xADU, 0xC8U);
            if (status == UI_STATUS_OK) {
                status = gradient_rect((struct ui_rect){ icon.x, icon.y + 10U,
                    icon.width, icon.height - 10U }, damage,
                    0xB8U, 0xDCU, 0xEDU, 0x58U, 0x91U, 0xB0U);
            }
        } else {
            status = gradient_rect((struct ui_rect){ icon.x + 7U, icon.y,
                icon.width - 14U, icon.height }, damage,
                0xFFU, 0xFFU, 0xF9U, 0xD8U, 0xDEU, 0xDDU);
            for (uint32_t line = 0U; line < 4U && status == UI_STATUS_OK;
                 ++line) {
                status = fill_clipped((struct ui_rect){ icon.x + 14U,
                    icon.y + 12U + line * 7U, icon.width - 27U, 2U },
                    damage, state.theme.accent_teal);
            }
        }
        if (status == UI_STATUS_OK) {
            status = draw_text(tile, damage,
                centered_text_x(tile, entry->name), tile.y + 72U,
                entry->name, state.theme.ink);
        }
        if (status == UI_STATUS_OK && !entry->directory) {
            char size[32U];
            size_t size_at = append_u64(size, sizeof(size), 0U, entry->size);
            (void)append_text(size, sizeof(size), size_at, " bytes");
            status = draw_text(tile, damage, centered_text_x(tile, size),
                tile.y + 92U, size, state.theme.title_inactive);
        }
    }
    if (status == UI_STATUS_OK) {
        const struct sapfs_drive_info drive = sapfs_drive(SAPFS_VOLUME_DATA);
        char capacity[64U];
        size_t at = append_u64(capacity, sizeof(capacity), 0U,
            file_entry_count);

        at = append_text(capacity, sizeof(capacity), at,
            file_entry_count == 1U ? " item, " : " items, ");
        at = append_u64(capacity, sizeof(capacity), at,
            drive.free_bytes / (1024U * 1024U));
        (void)append_text(capacity, sizeof(capacity), at, " MB available");
        status = draw_text(status_bar, damage,
            centered_text_x(status_bar, capacity), status_bar.y + 17U,
            capacity, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(status_bar, damage, status_bar.x + 8U,
            status_bar.y + 17U, "data / fat32", state.theme.title_inactive);
    }
    return status;
}

static enum ui_status draw_notes_app(struct ui_rect damage)
{
    const struct ui_rect client = state.layout.panel_client;
    const struct ui_rect toolbar = { client.x, client.y, client.width, 42U };
    const struct ui_rect paper = { client.x + 24U, client.y + 54U,
        client.width - 48U, client.height - 84U };
    enum ui_status status = gradient_rect(toolbar, damage,
        0xF0U, 0xF2U, 0xF2U, 0xB4U, 0xBEU, 0xC1U);

    if (status == UI_STATUS_OK) {
        status = draw_button((struct ui_rect){ toolbar.x + 10U,
            toolbar.y + 8U, 72U, 26U }, damage, "Save");
    }
    if (status == UI_STATUS_OK) {
        char title[SAPFS_MAX_PATH + 16U];
        size_t at = append_text(title, sizeof(title), 0U, "Editing / ");
        (void)append_text(title, sizeof(title), at, note_path);
        status = draw_text(toolbar, damage, toolbar.x + 98U,
            toolbar.y + 26U, title, state.theme.ink);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(drop_shadow_draw_rect(paper, 5U), damage,
            state.theme.shadow);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped(paper, damage,
            framebuffer_pack(0xFFU, 0xFEU, 0xF4U));
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){ paper.x + 42U, paper.y,
            2U, paper.height }, damage,
            framebuffer_pack(0xE8U, 0x86U, 0x86U));
    }
    for (uint32_t y = paper.y + 28U; y < paper.y + paper.height &&
         status == UI_STATUS_OK; y += 22U) {
        status = fill_clipped((struct ui_rect){ paper.x + 10U, y,
            paper.width - 20U, 1U }, damage,
            framebuffer_pack(0xC5U, 0xD9U, 0xDFU));
    }
    size_t source = 0U;
    uint32_t line = 0U;
    while (source < note_length && line < 18U && status == UI_STATUS_OK) {
        char text[82U];
        size_t count = 0U;

        while (source < note_length && note_buffer[source] != '\n' &&
               count + 1U < sizeof(text)) {
            text[count++] = note_buffer[source++];
        }
        if (source < note_length && note_buffer[source] == '\n') {
            ++source;
        }
        text[count] = '\0';
        status = draw_text(paper, damage, paper.x + 54U,
            paper.y + 24U + line * 22U, text, state.theme.ink);
        ++line;
    }
    if (status == UI_STATUS_OK) {
        char status_line[96U];
        size_t at = append_text(status_line, sizeof(status_line), 0U,
            note_dirty ? "Unsaved / " : "Saved / ");
        (void)append_text(status_line, sizeof(status_line), at, app_status);
        status = draw_text(client, damage, client.x + 12U,
            client.y + client.height - 8U, status_line, state.theme.ink);
    }
    return status;
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
            state.active_panel == UI_PANEL_TERMINAL ?
            framebuffer_pack(0x08U, 0x10U, 0x12U) : state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = bevel_clipped(state.layout.panel_client, damage, false);
    }
    if (status != UI_STATUS_OK) {
        return status;
    }

    if (state.active_panel == UI_PANEL_FILES) {
        return draw_files_app(damage);
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
    if (state.active_panel == UI_PANEL_NOTES) {
        return draw_notes_app(damage);
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
            const uint32_t bit = UINT32_C(0x80000000) >> x;
            uint32_t pixel;

            if ((cursor_outer[y] & bit) == 0U) {
                continue;
            }
            pixel = (cursor_inner[y] & bit) != 0U ?
                state.theme.ink : state.theme.white;
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
    return draw_wallpaper(damage);
}

static enum ui_status draw_dock_shelf(struct ui_rect damage)
{
    const struct ui_rect dock = state.layout.dock;
    enum ui_status status = translucent_fill((struct ui_rect){ dock.x + 4U,
        dock.y + 78U, dock.width - 8U, 43U }, damage,
        state.theme.shadow, 150U);

    for (uint32_t row = 0U; row < 50U && status == UI_STATUS_OK; ++row) {
        const uint32_t inset = (49U - row) * 24U / 49U;
        const uint8_t shade = (uint8_t)(218U - row * 2U);
        const struct ui_rect strip = { dock.x + inset,
            dock.y + 72U + row, dock.width - inset * 2U, 1U };

        status = translucent_fill(strip, damage,
            framebuffer_pack(shade, (uint8_t)(shade + 5U),
                (uint8_t)(shade + 8U)), row < 9U ? 152U : 218U);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){ dock.x + 24U,
            dock.y + 71U, dock.width - 48U, 2U }, damage,
            state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = translucent_fill((struct ui_rect){ dock.x + 2U,
            dock.y + 120U, dock.width - 4U, 4U }, damage,
            framebuffer_pack(0x32U, 0x3BU, 0x40U), 230U);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){ dock.x + 12U,
            dock.y + 118U, dock.width - 24U, 1U }, damage,
            framebuffer_pack(0xD5U, 0xE4U, 0xE8U));
    }
    return status;
}

static enum ui_status draw_menu_brand(struct ui_rect damage)
{
    enum ui_status status = draw_logo_color((struct ui_rect){ 8U, 3U,
        18U, 17U }, damage);
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage, 34U,
            state.layout.menu_baseline, "Sapote First Environment",
            state.theme.white);
    }
    if (status == UI_STATUS_OK) {
        status = draw_text(state.layout.menu_bar, damage,
            state.layout.surface.width - 134U, state.layout.menu_baseline,
            "Sapote 2.0.0", state.theme.white);
    }
    return status;
}

static enum ui_status render_region(struct ui_rect damage, bool full)
{
    enum ui_status status = draw_desktop_pattern(damage);

    if (status == UI_STATUS_OK) {
        status = translucent_fill(state.layout.menu_bar, damage,
            state.theme.shadow, 210U);
    }
    if (status == UI_STATUS_OK) {
        status = fill_clipped((struct ui_rect){
            state.layout.menu_bar.x,
            state.layout.menu_bar.y + state.layout.menu_bar.height - 1U,
            state.layout.menu_bar.width, 1U
        }, damage, framebuffer_pack(0x8AU, 0xAEU, 0xB8U));
    }
    if (status == UI_STATUS_OK) {
        status = draw_menu_brand(damage);
    }
    if (status == UI_STATUS_OK) {
        status = draw_dock_shelf(damage);
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
    uint32_t wallpaper_width;
    uint32_t wallpaper_height;
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
    if (sapote_wallpaper_geometry(&wallpaper_width, &wallpaper_height) !=
            WALLPAPER_STATUS_OK || wallpaper_width != 1024U ||
        wallpaper_height != 768U ||
        sapote_wallpaper_decode(wallpaper_pixels, 1024U * 768U,
            framebuffer.red_position, framebuffer.green_position,
            framebuffer.blue_position) != WALLPAPER_STATUS_OK) {
        canvas = NULL;
        return UI_STATUS_WALLPAPER_FAILURE;
    }

    install_theme(&state.theme);
    state.pointer_present = pointer_present;
    state.focus = UI_ELEMENT_DOCK_FILES;
    state.hover = UI_ELEMENT_NONE;
    state.pressed = UI_ELEMENT_NONE;
    state.active_panel = UI_PANEL_NONE;
    state.ledger_pass = false;
    terminal_welcomed = false;
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

    if (screen_set_palette(0x08U, 0x10U, 0x12U,
            0x9DU, 0xD7U, 0xA3U) != SCREEN_STATUS_OK ||
        screen_set_deferred_present(true) != SCREEN_STATUS_OK ||
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
    if (state.active_panel == UI_PANEL_NOTES && event->scancode != 0x01U) {
        ui_event.type = UI_EVENT_TEXT_INPUT;
        ui_event.control = event->control;
        if (event->control && (event->character == 's' ||
                event->character == 'S')) {
            ui_event.character = 's';
        } else if (event->scancode == 0x0EU) {
            ui_event.character = '\b';
        } else if (event->scancode == 0x1CU) {
            ui_event.character = '\n';
        } else if (!event->control && event->character >= ' ' &&
                event->character <= '~') {
            ui_event.character = event->character;
        } else {
            return UI_STATUS_OK;
        }
    } else if (event->scancode == 0x0FU) {
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
    if (current < UI_ELEMENT_DOCK_FILES || current > UI_ELEMENT_DOCK_NOTES) {
        return UI_ELEMENT_DOCK_FILES;
    }
    if (previous) {
        return current == UI_ELEMENT_DOCK_FILES ? UI_ELEMENT_DOCK_NOTES :
            (enum ui_element_id)(current - 1);
    }
    return current == UI_ELEMENT_DOCK_NOTES ? UI_ELEMENT_DOCK_FILES :
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
        return UI_STATUS_OK;
    }
    if (old_panel == UI_PANEL_NOTES && note_dirty) {
        (void)note_save();
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

    if (panel == UI_PANEL_FILES) {
        (void)files_refresh();
    } else if (panel == UI_PANEL_NOTES) {
        (void)note_load();
    }

    if (panel == UI_PANEL_TERMINAL) {
        if (screen_set_viewport(surface_rect_of(state.layout.panel_client),
                true) != SCREEN_STATUS_OK) {
            return UI_STATUS_SCREEN_FAILURE;
        }
        if (!terminal_welcomed) {
            if (screen_clear() != SCREEN_STATUS_OK ||
                screen_write("Sapote terminal\n"
                    "Type help for commands. Type fetch for system identity.\n"
                    "\nsap> ") != SCREEN_STATUS_OK) {
                return UI_STATUS_SCREEN_FAILURE;
            }
            terminal_welcomed = true;
        }
    } else if (screen_set_visible(false) != SCREEN_STATUS_OK) {
        return UI_STATUS_SCREEN_FAILURE;
    }
    return UI_STATUS_OK;
}

static enum ui_element_id active_hit(struct ui_point point)
{
    enum ui_element_id hit = UI_ELEMENT_NONE;

    if (ui_hit_test(&state.layout, point, &hit) != UI_STATUS_OK ||
        hit != UI_ELEMENT_NONE) {
        return hit;
    }
    if (state.active_panel == UI_PANEL_NONE) {
        return UI_ELEMENT_NONE;
    }
    const struct ui_rect close = { state.layout.panel.x + 10U,
        state.layout.panel.y + 8U, 18U, 18U };
    if (rect_contains_point(close, point)) {
        return UI_ELEMENT_WINDOW_CLOSE;
    }
    const struct ui_rect client = state.layout.panel_client;
    if (state.active_panel == UI_PANEL_FILES) {
        const struct ui_rect buttons[] = {
            { client.x + 10U, client.y + 8U, 56U, 26U },
            { client.x + 76U, client.y + 8U, 88U, 26U },
            { client.x + 174U, client.y + 8U, 104U, 26U },
            { client.x + client.width - 156U, client.y + 8U, 68U, 26U },
            { client.x + client.width - 78U, client.y + 8U, 68U, 26U }
        };
        static const enum ui_element_id ids[] = {
            UI_ELEMENT_FILES_UP, UI_ELEMENT_FILES_NEW_FILE,
            UI_ELEMENT_FILES_NEW_FOLDER, UI_ELEMENT_FILES_REFRESH,
            UI_ELEMENT_FILES_SYNC
        };
        for (size_t index = 0U; index < 5U; ++index) {
            if (rect_contains_point(buttons[index], point)) {
                return ids[index];
            }
        }
        for (size_t index = 0U; index < file_entry_count && index < 12U;
             ++index) {
            if (rect_contains_point(file_entry_rect(index), point)) {
                return (enum ui_element_id)(UI_ELEMENT_FILES_ENTRY_0 + index);
            }
        }
    } else if (state.active_panel == UI_PANEL_NOTES &&
        rect_contains_point((struct ui_rect){ client.x + 10U,
            client.y + 8U, 72U, 26U }, point)) {
        return UI_ELEMENT_NOTES_SAVE;
    }
    return UI_ELEMENT_NONE;
}

static enum ui_status activate_element(
    enum ui_element_id element,
    struct ui_rect *damage
)
{
    if (element >= UI_ELEMENT_DOCK_FILES &&
        element <= UI_ELEMENT_DOCK_NOTES) {
        return set_panel(panel_for_element(element), damage);
    }
    if (element == UI_ELEMENT_WINDOW_CLOSE) {
        return set_panel(UI_PANEL_NONE, damage);
    }
    if (element == UI_ELEMENT_FILES_UP) {
        files_up();
    } else if (element == UI_ELEMENT_FILES_NEW_FILE) {
        files_create(false);
    } else if (element == UI_ELEMENT_FILES_NEW_FOLDER) {
        files_create(true);
    } else if (element == UI_ELEMENT_FILES_REFRESH) {
        (void)files_refresh();
    } else if (element == UI_ELEMENT_FILES_SYNC) {
        set_app_status("sync", sapfs_sync(SAPFS_VOLUME_DATA));
    } else if (element >= UI_ELEMENT_FILES_ENTRY_0 &&
            element <= UI_ELEMENT_FILES_ENTRY_11) {
        const size_t index = (size_t)(element - UI_ELEMENT_FILES_ENTRY_0);
        char path[SAPFS_MAX_PATH + 1U];

        if (index >= file_entry_count ||
            !entry_path(file_entries[index].name, path)) {
            return UI_STATUS_BAD_ELEMENT;
        }
        if (file_entries[index].directory) {
            if (!copy_string(file_directory, sizeof(file_directory), path)) {
                return UI_STATUS_FILESYSTEM_FAILURE;
            }
            (void)files_refresh();
        } else {
            if (!copy_string(note_path, sizeof(note_path), path)) {
                return UI_STATUS_FILESYSTEM_FAILURE;
            }
            (void)note_load();
            return set_panel(UI_PANEL_NOTES, damage);
        }
    } else if (element == UI_ELEMENT_NOTES_SAVE) {
        (void)note_save();
    } else {
        return UI_STATUS_BAD_ELEMENT;
    }
    *damage = rect_union(*damage, state.layout.panel);
    return UI_STATUS_OK;
}

static void note_input(char character, bool control)
{
    if (control && (character == 's' || character == 'S')) {
        (void)note_save();
        return;
    }
    if (character == '\b') {
        if (note_length != 0U) {
            --note_length;
            note_buffer[note_length] = '\0';
            note_dirty = true;
        }
        return;
    }
    if ((character == '\n' || (character >= ' ' && character <= '~')) &&
        note_length + 1U < sizeof(note_buffer)) {
        note_buffer[note_length++] = character;
        note_buffer[note_length] = '\0';
        note_dirty = true;
        set_app_status("editing in memory", SAPFS_STATUS_OK);
    } else if (note_length + 1U >= sizeof(note_buffer)) {
        set_app_status("note capacity reached", SAPFS_STATUS_RANGE);
    }
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
        hit = active_hit(event->point);
        state.pressed = hit;
        *damage = rect_union(*damage,
            hit >= UI_ELEMENT_DOCK_FILES && hit <= UI_ELEMENT_DOCK_NOTES ?
            dock_bounds_for(&state.layout, hit) : state.layout.panel);
        state.renders.dock_state_changes += 1U;
    } else if (event->type == UI_EVENT_POINTER_BUTTON_RELEASE &&
        event->button == UI_POINTER_BUTTON_LEFT) {
        const enum ui_element_id pressed = state.pressed;

        hit = active_hit(event->point);
        state.pressed = UI_ELEMENT_NONE;
        *damage = rect_union(*damage,
            pressed >= UI_ELEMENT_DOCK_FILES &&
                pressed <= UI_ELEMENT_DOCK_NOTES ?
            dock_bounds_for(&state.layout, pressed) : state.layout.panel);
        if (pressed != UI_ELEMENT_NONE && pressed == hit) {
            const enum ui_status status = activate_element(pressed, damage);

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
    } else if (event->type == UI_EVENT_TEXT_INPUT &&
        state.active_panel == UI_PANEL_NOTES) {
        note_input(event->character, event->control);
        *damage = rect_union(*damage, state.layout.panel);
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
            if ((cursor_outer[y] &
                    (UINT32_C(0x80000000) >> x)) != 0U) {
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

    self_test_failure = "First Environment UI self-test passed";
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
    damaged.panel.x = UINT32_MAX - 3U;
    damaged.panel.width = 8U;
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
    if (both.x != 0U || both.y != 0U || both.width != 127U ||
        both.height != 137U || UI_CURSOR_HOTSPOT_X >= UI_CURSOR_WIDTH ||
        UI_CURSOR_HOTSPOT_Y >= UI_CURSOR_HEIGHT) {
        self_test_failure = "cursor damage union or hotspot is invalid";
        return false;
    }
    if (next_focus(UI_ELEMENT_DOCK_FILES, true) !=
            UI_ELEMENT_DOCK_NOTES ||
        next_focus(UI_ELEMENT_DOCK_NOTES, false) !=
            UI_ELEMENT_DOCK_FILES) {
        self_test_failure = "keyboard focus wrap is invalid";
        return false;
    }
    if (!event_queue_self_test()) {
        self_test_failure = event_queue_failure;
        return false;
    }

    const uint64_t stable = synthetic_render_hash(false);
    if (stable != UINT64_C(0x95DF9F60511EDCD5) ||
        stable == synthetic_render_hash(true)) {
        self_test_failure = "synthetic First Environment render hash is invalid";
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
    if (state.theme.white != framebuffer_pack(0xF8U, 0xFAU, 0xF8U) ||
        state.theme.ink != framebuffer_pack(0x18U, 0x21U, 0x24U) ||
        state.theme.desktop_dark != framebuffer_pack(0x07U, 0x16U, 0x22U) ||
        state.theme.desktop_light != framebuffer_pack(0x1CU, 0x4BU, 0x5AU) ||
        state.theme.title_active != framebuffer_pack(0x1CU, 0x29U, 0x2DU) ||
        state.theme.title_inactive != framebuffer_pack(0x91U, 0x9DU, 0xA2U) ||
        state.theme.accent_teal != framebuffer_pack(0x68U, 0xA9U, 0xC5U) ||
        state.theme.accent_gold != framebuffer_pack(0xE6U, 0xC4U, 0x62U) ||
        state.theme.accent_green != framebuffer_pack(0x8EU, 0xADU, 0x89U) ||
        state.theme.accent_red != framebuffer_pack(0xD9U, 0x55U, 0x4FU) ||
        state.theme.accent_violet != framebuffer_pack(0x94U, 0x7BU, 0xB4U) ||
        state.theme.shadow != framebuffer_pack(0x05U, 0x0CU, 0x12U) ||
        state.theme.window_face != framebuffer_pack(0xD9U, 0xDFU, 0xE0U)) {
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
        "None", "Files", "Terminal", "Notes"
    };

    if ((size_t)panel >= sizeof(names) / sizeof(names[0])) {
        return "Unknown panel";
    }
    return names[panel];
}

const char *ui_element_name(enum ui_element_id element)
{
    if (element == UI_ELEMENT_NONE) {
        return "none";
    }
    if (element == UI_ELEMENT_DOCK_FILES) {
        return "Files";
    }
    if (element == UI_ELEMENT_DOCK_TERMINAL) {
        return "Terminal";
    }
    if (element == UI_ELEMENT_DOCK_NOTES) {
        return "Notes";
    }
    return element < UI_ELEMENT_COUNT ? "application control" :
        "unknown element";
}

const char *ui_status_string(enum ui_status status)
{
    static const char *const messages[] = {
        "ok",
        "null UI argument",
        "First Environment is already initialized",
        "First Environment is not initialized",
        "First Environment is not active",
        "unsupported First Environment framebuffer geometry",
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
        "desktop wallpaper rendering failed",
        "application filesystem operation failed",
        "terminal viewport rendering failed",
        "installed First Environment proof failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        (size_t)UI_STATUS_INSTALLED_PROOF_FAILURE + 1U,
        "UI status messages are out of sync");
    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown UI status";
    }
    return messages[status];
}
