/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include "input_processor_gestures.h"
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(gestures, CONFIG_ZMK_LOG_LEVEL);

int touch_detection_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                               uint32_t param2, struct zmk_input_processor_state *state) {
    struct gesture_config *config = (struct gesture_config *)dev->config;
    struct gesture_data *data = (struct gesture_data *)dev->data;
    k_work_reschedule(&data->touch_detection.touch_end_timeout_work, K_MSEC(config->touch_detection.wait_for_new_position_ms));

    if (event->type != INPUT_EV_ABS && event->type == INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    data->touch_detection.complete = !data->touch_detection.complete;

    if (data->touch_detection.complete && data->touch_detection.absolute != (event->type == INPUT_EV_ABS)) {
        LOG_ERR("Surprising change of absolute/relative type. It's now [%s] but it's supposed to be [%s]. Don't know how to handle that, so ignoring this", 
            event->type == INPUT_EV_ABS ? "absolute" : "relative",
            data->touch_detection.absolute ? "absolute" : "relative"
        );
        return ZMK_INPUT_PROC_CONTINUE;
    } else {
        data->touch_detection.absolute = (event->type == INPUT_EV_ABS);
    }

    if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
        data->touch_detection.x = event->value;
    } else if (event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y) {
        data->touch_detection.y = event->value;
    }

    // 억제 조건 판별: 스크롤 중이거나 탭 대기 중일 때
    bool should_suppress = data->circular_scroll.is_tracking || 
                           (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap);

    if (! data->touch_detection.complete) {
        data->touch_detection.previous_event = event;

        // 원본의 스크롤 억제 방식을 그대로 사용하여 첫 번째 패킷(X) 변조
        if (should_suppress) {
            if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
                event->code = INPUT_REL_X;
            } else {
                event->code = INPUT_REL_Y;
            }
            event->type = INPUT_EV_REL;
            event->value = 0;
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint32_t now = k_uptime_get();

    // 커서 튀기 방지: 새로운 터치 시작 시 기준점을 즉시 동기화
    if (!data->touch_detection.touching) {
        data->touch_detection.previous_x = data->touch_detection.x;
        data->touch_detection.previous_y = data->touch_detection.y;
    }

    struct gesture_event_t gesture_event = {
        .last_touch_timestamp = now,
        .previous_touch_timestamp = data->touch_detection.last_touch_timestamp,
        .x = data->touch_detection.x,
        .y = data->touch_detection.y,
        .previous_x = data->touch_detection.previous_x,
        .previous_y = data->touch_detection.previous_y,
        .delta_x = data->touch_detection.x - data->touch_detection.previous_x,
        .delta_y = data->touch_detection.y - data->touch_detection.previous_y,
        .delta_time = now - data->touch_detection.last_touch_timestamp,
        .absolute = data->touch_detection.absolute,
        .raw_event_1 = data->touch_detection.previous_event,
        .raw_event_2 = event
    };

    data->touch_detection.last_touch_timestamp = now;

    // 움직임 탈출: 10유닛 이상 이동 시 탭 고정 해제
    if (data->tap_detection.is_waiting_for_tap && 
       (gesture_event.delta_x > 10 || gesture_event.delta_x < -10 || 
        gesture_event.delta_y > 10 || gesture_event.delta_y < -10)) {
        data->tap_detection.is_waiting_for_tap = false;
        // 탭 대기가 풀렸으므로 스크롤 상태만으로 억제 조건 재확인
        should_suppress = data->circular_scroll.is_tracking;
    }

    if (!data->touch_detection.touching){
        data->touch_detection.touching = true;
        
        // 오토 레이어 기능 삽입
        if (config->tap_detection.touch_layer >= 0) {
            bool scroller_active = false;
            for (int i = 0; i < config->tap_detection.ignore_layers_len; i++) {
                if (zmk_keymap_layer_active((uint8_t)config->tap_detection.ignore_layers[i])) {
                    scroller_active = true; break;
                }
            }
            if (!scroller_active) {
                zmk_keymap_layer_activate((uint8_t)config->tap_detection.touch_layer);
                data->touch_detection.auto_layer_active = true;
            }
        }
        config->handle_touch_start(dev, &gesture_event);
    } else {
        config->handle_touch_continue(dev, &gesture_event);
    }

    data->touch_detection.previous_x = data->touch_detection.x;
    data->touch_detection.previous_y = data->touch_detection.y;

    // 원본의 스크롤 억제 방식을 두 번째 패킷(Y)에도 적용하여 유출 완전 차단
    if (should_suppress) {
        if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
            event->code = INPUT_REL_X;
        } else {
            event->code = INPUT_REL_Y;
        }
        event->type = INPUT_EV_REL;
        event->value = 0;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

void touch_end_timeout_callback(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    struct touch_detection_data *data = CONTAINER_OF(d_work, struct touch_detection_data, touch_end_timeout_work);
    const struct device *dev = data->all->dev;
    struct gesture_config *config = (struct gesture_config *)dev->config;
    
    data->touching = false;
    if (data->auto_layer_active) {
        zmk_keymap_layer_deactivate((uint8_t)config->tap_detection.touch_layer);
        data->auto_layer_active = false;
    }
    data->complete = true;
    config->handle_touch_end(dev);
}

int touch_detection_init(const struct device *dev) {
    struct gesture_data *data = (struct gesture_data *)dev->data;
    data->touch_detection.last_touch_timestamp = k_uptime_get();
    data->touch_detection.complete = true;
    k_work_init_delayable(&data->touch_detection.touch_end_timeout_work, touch_end_timeout_callback);
    return 0;
}
