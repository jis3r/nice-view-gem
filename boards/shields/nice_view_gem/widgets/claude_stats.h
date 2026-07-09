#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

void claude_stats_init(lv_obj_t *parent, lv_color_t middle_cbuf[]);
void claude_stats_update_from_relay(uint8_t session_remaining, uint8_t weekly_remaining,
                                    uint16_t reset_minutes, bool central_stale);
