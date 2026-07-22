#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <raw_hid/events.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/split/central.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define CLAUDE_STATS_REPORT_LENGTH 32U
#define CLAUDE_STATS_REPORT_TYPE 0xB2U
#define CLAUDE_STATS_STALE_FLAG BIT(0)
#define CLAUDE_STATS_SAMPLE_VALID_FLAG BIT(1)
#define CLAUDE_STATS_EXTRA_ENABLED_FLAG BIT(2)
#define CLAUDE_STATS_ALLOWED_FLAGS                                                                 \
    (CLAUDE_STATS_STALE_FLAG | CLAUDE_STATS_SAMPLE_VALID_FLAG | CLAUDE_STATS_EXTRA_ENABLED_FLAG)
#define CLAUDE_STATS_ERROR_NONE 0U
#define CLAUDE_STATS_ERROR_API 3U
#define CLAUDE_STATS_PERIPHERAL_SOURCE 0U
#define CLAUDE_STATS_BEHAVIOR_NAME "cstats"

struct claude_stats_report {
    uint8_t type;
    uint8_t session_remaining;
    uint8_t weekly_remaining;
    uint16_t reset_minutes;
    uint8_t flags;
    uint8_t error;
    uint8_t extra_remaining;
    uint16_t extra_remaining_euros;
    int64_t received_at;
    bool valid;
};

static struct claude_stats_report latest_report;

K_MUTEX_DEFINE(latest_report_mutex);

static bool report_is_valid(const struct raw_hid_received_event *event) {
    if (event == NULL || event->data == NULL || event->length != CLAUDE_STATS_REPORT_LENGTH ||
        event->data[0] != CLAUDE_STATS_REPORT_TYPE || event->data[1] > 100U ||
        event->data[2] > 100U || (event->data[5] & ~CLAUDE_STATS_ALLOWED_FLAGS) != 0U ||
        event->data[6] > CLAUDE_STATS_ERROR_API || event->data[7] > 100U) {
        return false;
    }

    uint16_t reset_minutes = (uint16_t)event->data[3] | ((uint16_t)event->data[4] << 8U);
    uint16_t extra_remaining_euros = (uint16_t)event->data[8] | ((uint16_t)event->data[9] << 8U);
    if (reset_minutes > CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_MAX_RESET_MINUTES) {
        return false;
    }

    bool sample_valid = (event->data[5] & CLAUDE_STATS_SAMPLE_VALID_FLAG) != 0U;
    bool extra_enabled = (event->data[5] & CLAUDE_STATS_EXTRA_ENABLED_FLAG) != 0U;
    if ((!sample_valid && (event->data[1] != 0U || event->data[2] != 0U || reset_minutes != 0U ||
                           event->data[7] != 0U || extra_remaining_euros != 0U || extra_enabled ||
                           event->data[6] == CLAUDE_STATS_ERROR_NONE)) ||
        (!extra_enabled && (event->data[7] != 0U || extra_remaining_euros != 0U))) {
        return false;
    }

    for (size_t index = 10U; index < CLAUDE_STATS_REPORT_LENGTH; index++) {
        if (event->data[index] != 0U) {
            return false;
        }
    }

    return true;
}

static uint32_t pack_param1(const struct claude_stats_report *report) {
    return (uint32_t)report->session_remaining | ((uint32_t)report->weekly_remaining << 8U) |
           ((uint32_t)report->reset_minutes << 16U);
}

static uint32_t pack_param2(const struct claude_stats_report *report) {
    return (uint32_t)report->flags | ((uint32_t)report->error << 3U) |
           ((uint32_t)report->extra_remaining << 5U) |
           ((uint32_t)report->extra_remaining_euros << 12U);
}

static void relay_latest_report(void) {
    struct claude_stats_report report;

    k_mutex_lock(&latest_report_mutex, K_FOREVER);
    report = latest_report;
    k_mutex_unlock(&latest_report_mutex);

    if (!report.valid || report.type != CLAUDE_STATS_REPORT_TYPE) {
        return;
    }

    if (k_uptime_get() - report.received_at >=
        (int64_t)CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_STALE_TIMEOUT_S * 1000LL) {
        report.flags |= CLAUDE_STATS_STALE_FLAG;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = CLAUDE_STATS_BEHAVIOR_NAME,
        .param1 = pack_param1(&report),
        .param2 = pack_param2(&report),
    };
    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
        .source = CLAUDE_STATS_PERIPHERAL_SOURCE,
    };

    int err =
        zmk_split_central_invoke_behavior(CLAUDE_STATS_PERIPHERAL_SOURCE, &binding, event, true);
    if (err < 0) {
        LOG_DBG("Claude stats relay unavailable (%d)", err);
    }
}

static void relay_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    relay_latest_report();
}
K_WORK_DEFINE(relay_work, relay_work_handler);

static void heartbeat_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_handler);

static void heartbeat_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_work_submit(&relay_work);
    k_work_reschedule(&heartbeat_work, K_SECONDS(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_HEARTBEAT_S));
}

static int raw_hid_received_event_listener(const zmk_event_t *eh) {
    const struct raw_hid_received_event *event = as_raw_hid_received_event(eh);
    if (!report_is_valid(event)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct claude_stats_report report = {
        .type = event->data[0],
        .session_remaining = event->data[1],
        .weekly_remaining = event->data[2],
        .reset_minutes = (uint16_t)event->data[3] | ((uint16_t)event->data[4] << 8U),
        .flags = event->data[5],
        .error = event->data[6],
        .extra_remaining = event->data[7],
        .extra_remaining_euros = (uint16_t)event->data[8] | ((uint16_t)event->data[9] << 8U),
        .received_at = k_uptime_get(),
        .valid = true,
    };

    k_mutex_lock(&latest_report_mutex, K_FOREVER);
    latest_report = report;
    k_mutex_unlock(&latest_report_mutex);

    k_work_submit(&relay_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(claude_stats_raw_hid, raw_hid_received_event_listener);
ZMK_SUBSCRIPTION(claude_stats_raw_hid, raw_hid_received_event);

static int claude_stats_relay_init(void) {
    k_work_reschedule(&heartbeat_work, K_SECONDS(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_HEARTBEAT_S));
    return 0;
}

SYS_INIT(claude_stats_relay_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
