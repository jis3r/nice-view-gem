#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>

#include "../nice_view_gem/widgets/claude_stats.h"

bool zmk_split_bt_peripheral_is_connected(void) { return true; }

uint8_t zmk_battery_state_of_charge(void) { return 95; }

ZMK_EVENT_IMPL(zmk_battery_state_changed);

static void preview_update_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(preview_update_work, preview_update_handler);

static void preview_update_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!zmk_display_is_initialized()) {
        k_work_reschedule(&preview_update_work, K_MSEC(50));
        return;
    }

    claude_stats_update_from_relay(62, 87, 102, false);
    raise_zmk_battery_state_changed((struct zmk_battery_state_changed){.state_of_charge = 95});
    raise_zmk_split_peripheral_status_changed(
        (struct zmk_split_peripheral_status_changed){.connected = true});
}

static int preview_init(void) {
    k_work_reschedule(&preview_update_work, K_MSEC(200));
    return 0;
}

SYS_INIT(preview_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
