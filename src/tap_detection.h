/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "input_processor_gestures.h"

struct tap_detection_data {
    bool is_waiting_for_tap;
    bool is_in_drag_window;
    bool is_dragging;
    uint8_t tap_button;
    struct k_work_delayable tap_timeout_work;
    struct k_work_delayable drag_window_work;
    gesture_data *all;
};

struct tap_detection_config {
    const bool enabled;
    const bool prevent_movement_during_tap;
    const bool tap_to_drag;
    const uint8_t tap_timout_ms;
    const uint16_t tap_drag_window_ms;
    const int right_click_layer;
    const int touch_layer; // auto_layer
    const int ignore_layers[8]; 
    const uint8_t ignore_layers_len;
};

handle_init_t tap_detection_init;
handle_touch_t tap_detection_handle_start;
handle_touch_t tap_detection_handle_touch;
handle_touch_end_t tap_detection_handle_end;

