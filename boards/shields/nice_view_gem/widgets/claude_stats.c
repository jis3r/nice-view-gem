#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zmk/display.h>

#include "claude_stats.h"
#include "../assets/custom_fonts.h"
#include "util.h"

#define CLAUDE_STATS_BAR_HEIGHT 4
#define CLAUDE_STATS_BAR_WIDTH 68
#define CLAUDE_STATS_STALE_FLAG BIT(0)
#define CLAUDE_STATS_SAMPLE_VALID_FLAG BIT(1)
#define CLAUDE_STATS_EXTRA_ENABLED_FLAG BIT(2)
#define CLAUDE_STATS_ERROR_NONE 0U
#define CLAUDE_STATS_ERROR_AUTH 1U
#define CLAUDE_STATS_ERROR_NETWORK 2U
#define CLAUDE_STATS_ERROR_API 3U

struct claude_stats_state {
    uint8_t session_remaining;
    uint8_t weekly_remaining;
    uint16_t reset_minutes;
    uint8_t flags;
    uint8_t error;
    uint8_t extra_remaining;
    uint16_t extra_remaining_euros;
    bool has_report;
    int64_t received_at;
};

static struct claude_stats_state state;
static lv_obj_t *middle_canvas;
static lv_color_t *middle_buffer;

K_MUTEX_DEFINE(claude_stats_mutex);

static void draw_label(lv_obj_t *canvas, int16_t x, int16_t y, int16_t width, lv_text_align_t align,
                       const char *text) {
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &pixel_operator_mono, align);
    lv_canvas_draw_text(canvas, x, y, width, &label_dsc, text);
}

static void draw_bar(lv_obj_t *canvas, int16_t y, uint8_t percent) {
    lv_draw_rect_dsc_t border_dsc;
    init_rect_dsc(&border_dsc, LVGL_BACKGROUND);
    border_dsc.bg_opa = LV_OPA_TRANSP;
    border_dsc.border_color = LVGL_FOREGROUND;
    border_dsc.border_width = 1;
    lv_canvas_draw_rect(canvas, 0, y, CLAUDE_STATS_BAR_WIDTH, CLAUDE_STATS_BAR_HEIGHT, &border_dsc);

    uint8_t fill_width = (uint8_t)(((CLAUDE_STATS_BAR_WIDTH - 2) * percent) / 100U);
    if (fill_width == 0U) {
        return;
    }

    lv_draw_rect_dsc_t fill_dsc;
    init_rect_dsc(&fill_dsc, LVGL_FOREGROUND);
    lv_canvas_draw_rect(canvas, 1, y + 1, fill_width, CLAUDE_STATS_BAR_HEIGHT - 2, &fill_dsc);
}

static void draw_euro(lv_obj_t *canvas, int16_t x, int16_t y) {
    lv_draw_rect_dsc_t glyph_dsc;
    init_rect_dsc(&glyph_dsc, LVGL_FOREGROUND);

    lv_canvas_draw_rect(canvas, x + 1, y + 1, 1, 7, &glyph_dsc);
    lv_canvas_draw_rect(canvas, x + 2, y, 3, 1, &glyph_dsc);
    lv_canvas_draw_rect(canvas, x + 2, y + 8, 3, 1, &glyph_dsc);
    lv_canvas_draw_rect(canvas, x, y + 3, 5, 1, &glyph_dsc);
    lv_canvas_draw_rect(canvas, x, y + 5, 5, 1, &glyph_dsc);
}

static const uint8_t tiny_glyphs[][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7}, {0x7, 0x1, 0x7, 0x4, 0x7},
    {0x7, 0x1, 0x7, 0x1, 0x7}, {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
    {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x1, 0x1, 0x1}, {0x7, 0x5, 0x7, 0x5, 0x7},
    {0x7, 0x5, 0x7, 0x1, 0x7}, {0x0, 0x2, 0x7, 0x2, 0x0},
};

static void draw_tiny_glyph(lv_obj_t *canvas, int16_t x, int16_t y, char character) {
    size_t glyph_index = character == '+' ? 10U : (size_t)(character - '0');
    lv_draw_rect_dsc_t glyph_dsc;
    init_rect_dsc(&glyph_dsc, LVGL_FOREGROUND);

    for (uint8_t row = 0U; row < 5U; row++) {
        for (uint8_t column = 0U; column < 3U; column++) {
            if ((tiny_glyphs[glyph_index][row] & BIT(2U - column)) != 0U) {
                lv_canvas_draw_rect(canvas, x + column, y + row, 1, 1, &glyph_dsc);
            }
        }
    }
}

static void draw_tiny_text(lv_obj_t *canvas, int16_t x, int16_t y, const char *text) {
    for (size_t index = 0U; text[index] != '\0'; index++) {
        draw_tiny_glyph(canvas, x + (int16_t)(index * 4U), y, text[index]);
    }
}

static const char *error_text(uint8_t error) {
    switch (error) {
    case CLAUDE_STATS_ERROR_AUTH:
        return "ERR AUTH";
    case CLAUDE_STATS_ERROR_NETWORK:
        return "ERR NET";
    case CLAUDE_STATS_ERROR_API:
        return "ERR API";
    default:
        return NULL;
    }
}

static bool stats_are_stale(const struct claude_stats_state *stats) {
    if ((stats->flags & CLAUDE_STATS_STALE_FLAG) != 0U) {
        return true;
    }

    return k_uptime_get() - stats->received_at >=
           (int64_t)CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_STALE_TIMEOUT_S * 1000LL;
}

static void draw_centered_status(const char *text) {
    draw_label(middle_canvas, 0, 24, BUFFER_SIZE, LV_TEXT_ALIGN_CENTER, text);
}

static void draw_extra_value(const struct claude_stats_state *stats) {
    if ((stats->flags & CLAUDE_STATS_EXTRA_ENABLED_FLAG) == 0U) {
        draw_label(middle_canvas, 30, 38, 38, LV_TEXT_ALIGN_RIGHT, "OFF");
        return;
    }

    char amount_text[6];
    if (stats->extra_remaining_euros > 9999U) {
        snprintk(amount_text, sizeof(amount_text), "9999+");
    } else {
        snprintk(amount_text, sizeof(amount_text), "%u", stats->extra_remaining_euros);
    }
    size_t amount_length = strlen(amount_text);
    int16_t amount_width = (int16_t)(amount_length * 4U - 1U);
    int16_t amount_x = BUFFER_SIZE - amount_width;
    draw_euro(middle_canvas, amount_x - 6, 40);
    draw_tiny_text(middle_canvas, amount_x, 42, amount_text);
}

static void draw_footer(const struct claude_stats_state *stats, bool stale) {
    if (stale) {
        draw_label(middle_canvas, 0, 57, BUFFER_SIZE, LV_TEXT_ALIGN_CENTER, "ERR HOST");
        return;
    }

    const char *explicit_error = error_text(stats->error);
    if (explicit_error != NULL) {
        draw_label(middle_canvas, 0, 57, BUFFER_SIZE, LV_TEXT_ALIGN_CENTER, explicit_error);
        return;
    }

    char reset_text[9];
    uint16_t hours = stats->reset_minutes / 60U;
    uint16_t minutes = stats->reset_minutes % 60U;
    snprintk(reset_text, sizeof(reset_text), "%u:%02u", hours, minutes);
    draw_label(middle_canvas, 0, 57, 24, LV_TEXT_ALIGN_LEFT, "RST");
    draw_label(middle_canvas, 24, 57, 44, LV_TEXT_ALIGN_RIGHT, reset_text);
}

static void draw_middle(const struct claude_stats_state *stats) {
    fill_background(middle_canvas);
    bool stale = stats_are_stale(stats);

    if (!stats->has_report) {
        draw_centered_status(stale ? "ERR HOST" : "SYNC");
        rotate_canvas(middle_canvas, middle_buffer);
        return;
    }

    if ((stats->flags & CLAUDE_STATS_SAMPLE_VALID_FLAG) == 0U) {
        draw_centered_status(stale ? "ERR HOST" : error_text(stats->error));
        rotate_canvas(middle_canvas, middle_buffer);
        return;
    }

    char session_text[5];
    char weekly_text[5];
    snprintk(session_text, sizeof(session_text), "%u%%", stats->session_remaining);
    snprintk(weekly_text, sizeof(weekly_text), "%u%%", stats->weekly_remaining);

    draw_label(middle_canvas, 0, 0, 36, LV_TEXT_ALIGN_LEFT, "SESH");
    draw_label(middle_canvas, 36, 0, 32, LV_TEXT_ALIGN_RIGHT, session_text);
    draw_bar(middle_canvas, 14, stats->session_remaining);

    draw_label(middle_canvas, 0, 19, 36, LV_TEXT_ALIGN_LEFT, "WEEK");
    draw_label(middle_canvas, 36, 19, 32, LV_TEXT_ALIGN_RIGHT, weekly_text);
    draw_bar(middle_canvas, 33, stats->weekly_remaining);

    draw_label(middle_canvas, 0, 38, 40, LV_TEXT_ALIGN_LEFT, "EXTRA");
    draw_extra_value(stats);
    draw_bar(middle_canvas, 52, stats->extra_remaining);

    draw_footer(stats, stale);
    rotate_canvas(middle_canvas, middle_buffer);
}

static void redraw(void) {
    struct claude_stats_state copy;

    k_mutex_lock(&claude_stats_mutex, K_FOREVER);
    copy = state;
    k_mutex_unlock(&claude_stats_mutex);

    if (middle_canvas == NULL) {
        return;
    }

    draw_middle(&copy);
}

static void redraw_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    redraw();
}
K_WORK_DEFINE(redraw_work, redraw_work_handler);

static void queue_redraw(void) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &redraw_work);
    }
}

static void stale_check_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(stale_check_work, stale_check_handler);

static void stale_check_handler(struct k_work *work) {
    ARG_UNUSED(work);
    queue_redraw();
    k_work_reschedule(&stale_check_work, K_SECONDS(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_HEARTBEAT_S));
}

void claude_stats_init(lv_obj_t *parent, lv_color_t middle_cbuf[]) {
    middle_buffer = middle_cbuf;

    k_mutex_lock(&claude_stats_mutex, K_FOREVER);
    state.received_at = k_uptime_get();
    k_mutex_unlock(&claude_stats_mutex);

    middle_canvas = lv_canvas_create(parent);
    lv_obj_align(middle_canvas, LV_ALIGN_TOP_RIGHT, BUFFER_OFFSET_MIDDLE, 0);
    lv_canvas_set_buffer(middle_canvas, middle_buffer, BUFFER_SIZE, BUFFER_SIZE,
                         LV_IMG_CF_TRUE_COLOR);

    redraw();
    k_work_reschedule(&stale_check_work, K_SECONDS(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_HEARTBEAT_S));
}

void claude_stats_update_from_relay(uint8_t session_remaining, uint8_t weekly_remaining,
                                    uint16_t reset_minutes, uint8_t flags, uint8_t error,
                                    uint8_t extra_remaining, uint16_t extra_remaining_euros) {
    k_mutex_lock(&claude_stats_mutex, K_FOREVER);
    state.session_remaining = session_remaining;
    state.weekly_remaining = weekly_remaining;
    state.reset_minutes = reset_minutes;
    state.flags = flags;
    state.error = error;
    state.extra_remaining = extra_remaining;
    state.extra_remaining_euros = extra_remaining_euros;
    state.has_report = true;
    state.received_at = k_uptime_get();
    k_mutex_unlock(&claude_stats_mutex);

    queue_redraw();
}
