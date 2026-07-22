#define DT_DRV_COMPAT nice_view_gem_behavior_claude_stats

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#if IS_ENABLED(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS) && IS_ENABLED(CONFIG_ZMK_SPLIT) &&               \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include "claude_stats.h"
#endif

#define CLAUDE_STATS_STALE_FLAG BIT(0)
#define CLAUDE_STATS_SAMPLE_VALID_FLAG BIT(1)
#define CLAUDE_STATS_EXTRA_ENABLED_FLAG BIT(2)
#define CLAUDE_STATS_ALLOWED_FLAGS                                                                 \
    (CLAUDE_STATS_STALE_FLAG | CLAUDE_STATS_SAMPLE_VALID_FLAG | CLAUDE_STATS_EXTRA_ENABLED_FLAG)
#define CLAUDE_STATS_ERROR_API 3U
#define CLAUDE_STATS_PARAM2_RESERVED_MASK 0xF0000000U

#if IS_ENABLED(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS)
#define CLAUDE_STATS_MAX_RESET_MINUTES CONFIG_NICE_VIEW_GEM_CLAUDE_STATS_MAX_RESET_MINUTES
#else
#define CLAUDE_STATS_MAX_RESET_MINUTES 10080U
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int cstats_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    uint8_t session_remaining = binding->param1 & 0xFFU;
    uint8_t weekly_remaining = (binding->param1 >> 8U) & 0xFFU;
    uint16_t reset_minutes = (binding->param1 >> 16U) & 0xFFFFU;
    uint8_t flags = binding->param2 & 0x07U;
    uint8_t error = (binding->param2 >> 3U) & 0x03U;
    uint8_t extra_remaining = (binding->param2 >> 5U) & 0x7FU;
    uint16_t extra_remaining_euros = (binding->param2 >> 12U) & 0xFFFFU;

    if (session_remaining > 100U || weekly_remaining > 100U ||
        reset_minutes > CLAUDE_STATS_MAX_RESET_MINUTES ||
        (flags & ~CLAUDE_STATS_ALLOWED_FLAGS) != 0U || error > CLAUDE_STATS_ERROR_API ||
        extra_remaining > 100U || (binding->param2 & CLAUDE_STATS_PARAM2_RESERVED_MASK) != 0U ||
        (((flags & CLAUDE_STATS_SAMPLE_VALID_FLAG) == 0U) &&
         (session_remaining != 0U || weekly_remaining != 0U || reset_minutes != 0U ||
          extra_remaining != 0U || extra_remaining_euros != 0U ||
          (flags & CLAUDE_STATS_EXTRA_ENABLED_FLAG) != 0U || error == 0U)) ||
        (((flags & CLAUDE_STATS_EXTRA_ENABLED_FLAG) == 0U) &&
         (extra_remaining != 0U || extra_remaining_euros != 0U))) {
        return -EINVAL;
    }

#if IS_ENABLED(CONFIG_NICE_VIEW_GEM_CLAUDE_STATS) && IS_ENABLED(CONFIG_ZMK_SPLIT) &&               \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    claude_stats_update_from_relay(session_remaining, weekly_remaining, reset_minutes, flags, error,
                                   extra_remaining, extra_remaining_euros);
#endif

    return ZMK_BEHAVIOR_OPAQUE;
}

static int cstats_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api cstats_driver_api = {
    .binding_pressed = cstats_binding_pressed,
    .binding_released = cstats_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &cstats_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
