#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zmk/display.h>

#include "claude_stats.h"
#include "../assets/custom_fonts.h"
#include "util.h"

#define CLAUDE_STATS_BAR_HEIGHT 4
#define CLAUDE_STATS_BAR_WIDTH 68
#define CLAUDE_STATS_STALE_MARKER_SIZE 3

struct claude_stats_state {
    uint8_t session_remaining;
    uint8_t weekly_remaining;
    uint16_t reset_minutes;
    bool central_stale;
    bool valid;
    int64_t received_at;
};

static struct claude_stats_state state;
static lv_obj_t *middle_canvas;
static lv_color_t *middle_buffer;

K_MUTEX_DEFINE(claude_stats_mutex);

static void draw_label(lv_obj_t *canvas, int16_t x, int16_t y, int16_t width,
                       lv_text_align_t align, const char *text) {
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
    lv_canvas_draw_rect(canvas, 0, y, CLAUDE_STATS_BAR_WIDTH, CLAUDE_STATS_BAR_HEIGHT,
                        &border_dsc);

    uint8_t fill_width = (uint8_t)(((CLAUDE_STATS_BAR_WIDTH - 2) * percent) / 100U);
    if (fill_width == 0) {
        return;
    }

    lv_draw_rect_dsc_t fill_dsc;
    init_rect_dsc(&fill_dsc, LVGL_FOREGROUND);
    lv_canvas_draw_rect(canvas, 1, y + 1, fill_width, CLAUDE_STATS_BAR_HEIGHT - 2, &fill_dsc);
}

static bool stats_are_stale(const struct claude_stats_state *stats) {
    if (!stats->valid || stats->central_stale) {
        return true;
    }

    return k_uptime_get() - stats->received_at >=
           (int64_t)CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_STALE_TIMEOUT_S * 1000LL;
}

static void draw_middle(const struct claude_stats_state *stats) {
    fill_background(middle_canvas);

    if (!stats->valid) {
        draw_label(middle_canvas, 0, 0, BUFFER_SIZE, LV_TEXT_ALIGN_CENTER, "CLAUDE");
        draw_label(middle_canvas, 0, 13, BUFFER_SIZE, LV_TEXT_ALIGN_CENTER, "SYNC");
        rotate_canvas(middle_canvas, middle_buffer);
        return;
    }

    char session_text[5];
    char weekly_text[5];
    snprintk(session_text, sizeof(session_text), "%u%%", stats->session_remaining);
    snprintk(weekly_text, sizeof(weekly_text), "%u%%", stats->weekly_remaining);

    draw_label(middle_canvas, 0, 0, 62, LV_TEXT_ALIGN_LEFT, "CLAUDE");
    if (stats_are_stale(stats)) {
        lv_draw_rect_dsc_t stale_dsc;
        init_rect_dsc(&stale_dsc, LVGL_FOREGROUND);
        lv_canvas_draw_rect(middle_canvas, 65, 4, CLAUDE_STATS_STALE_MARKER_SIZE,
                            CLAUDE_STATS_STALE_MARKER_SIZE, &stale_dsc);
    }

    draw_label(middle_canvas, 0, 13, 36, LV_TEXT_ALIGN_LEFT, "SESH");
    draw_label(middle_canvas, 36, 13, 32, LV_TEXT_ALIGN_RIGHT, session_text);
    draw_bar(middle_canvas, 27, stats->session_remaining);

    draw_label(middle_canvas, 0, 32, 36, LV_TEXT_ALIGN_LEFT, "WEEK");
    draw_label(middle_canvas, 36, 32, 32, LV_TEXT_ALIGN_RIGHT, weekly_text);
    draw_bar(middle_canvas, 46, stats->weekly_remaining);

    char reset_text[9];
    uint16_t hours = stats->reset_minutes / 60U;
    uint16_t minutes = stats->reset_minutes % 60U;
    snprintk(reset_text, sizeof(reset_text), "%u:%02u", hours, minutes);
    draw_label(middle_canvas, 0, 52, 24, LV_TEXT_ALIGN_LEFT, "RST");
    draw_label(middle_canvas, 24, 52, 44, LV_TEXT_ALIGN_RIGHT, reset_text);

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
    k_work_reschedule(&stale_check_work,
                      K_SECONDS(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_HEARTBEAT_S));
}

void claude_stats_init(lv_obj_t *parent, lv_color_t middle_cbuf[]) {
    middle_buffer = middle_cbuf;

    middle_canvas = lv_canvas_create(parent);
    // Stats canvas x=48..115; 50% crystal x=2..36 leaves an 11px gap.
    lv_obj_align(middle_canvas, LV_ALIGN_TOP_RIGHT, BUFFER_OFFSET_MIDDLE, 0);
    lv_canvas_set_buffer(middle_canvas, middle_buffer, BUFFER_SIZE, BUFFER_SIZE,
                         LV_IMG_CF_TRUE_COLOR);

    redraw();
    k_work_reschedule(&stale_check_work,
                      K_SECONDS(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_HEARTBEAT_S));
}

void claude_stats_update_from_relay(uint8_t session_remaining, uint8_t weekly_remaining,
                                    uint16_t reset_minutes, bool central_stale) {
    k_mutex_lock(&claude_stats_mutex, K_FOREVER);
    state.session_remaining = session_remaining;
    state.weekly_remaining = weekly_remaining;
    state.reset_minutes = reset_minutes;
    state.central_stale = central_stale;
    state.valid = true;
    state.received_at = k_uptime_get();
    k_mutex_unlock(&claude_stats_mutex);

    queue_redraw();
}
