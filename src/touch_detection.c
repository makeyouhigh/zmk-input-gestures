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
    
    // [1] 좌표 신호 판별 (X, Y 축 신호만 필터링)
    bool is_coord = (event->code == INPUT_ABS_X || event->code == INPUT_REL_X || 
                     event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y);

    // 좌표가 아닌 신호(Sync 등)는 흐름을 깨지 않도록 무조건 통과 (버벅임 방지 핵심)
    if (!is_coord) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    k_work_reschedule(&data->touch_detection.touch_end_timeout_work, K_MSEC(config->touch_detection.wait_for_new_position_ms));

    // [2] 좌표 수집 및 타입 체크
    data->touch_detection.complete = !data->touch_detection.complete;
    if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
        data->touch_detection.x = event->value;
    } else if (event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y) {
        data->touch_detection.y = event->value;
    }

    // [3] 차단 조건 확인 (탭 대기 중이거나 써클 스크롤 중일 때)
    bool should_suppress = (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap) ||
                           (data->circular_scroll.is_tracking);

    // X축만 들어온 반쪽 패킷 처리
    if (! data->touch_detection.complete) {
        data->touch_detection.previous_event = event;
        if (should_suppress) {
            event->type = INPUT_EV_REL; event->value = 0; // 0점으로 속여서 전송
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // [4] Y축까지 들어온 완전한 패킷 처리
    uint32_t now = k_uptime_get();
    
    // 새로운 터치 시 좌표 동기화 (커서 튀기 방지)
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
        .delta_x = data->touch_detection.touching ? (data->touch_detection.x - data->touch_detection.previous_x) : 0,
        .delta_y = data->touch_detection.touching ? (data->touch_detection.y - data->touch_detection.previous_y) : 0,
        .delta_time = now - data->touch_detection.last_touch_timestamp,
        .absolute = (event->type == INPUT_EV_ABS),
        .raw_event_1 = data->touch_detection.previous_event,
        .raw_event_2 = event
    };

    data->touch_detection.last_touch_timestamp = now;

    // [5] 움직임 탈출 조건 (Threshold 3)
    if (data->tap_detection.is_waiting_for_tap && 
       (gesture_event.delta_x > 3 || gesture_event.delta_x < -3 || 
        gesture_event.delta_y > 3 || gesture_event.delta_y < -3)) {
        data->tap_detection.is_waiting_for_tap = false;
        should_suppress = data->circular_scroll.is_tracking;
    }

    // [6] 오토 레이어 제어
    if (!data->touch_detection.touching){
        data->touch_detection.touching = true;
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

    // [7] 최종 빗장: 탭/스크롤 중이면 마우스 이동값만 0으로 변조하여 전송
    if (should_suppress) {
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
