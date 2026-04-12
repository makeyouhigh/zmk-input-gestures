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
    
    // [1] 좌표 신호 여부 확인
    bool is_coord = (event->code == INPUT_ABS_X || event->code == INPUT_REL_X || 
                     event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y);

    // [2] 버벅임 방지: 탭 대기 중엔 Sync 신호를 포함한 모든 신호 전송 차단
    if (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap) {
        // 내부 좌표 정보는 계속 업데이트해야 하므로 아래 로직으로 이어짐
    } else if (!is_coord) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    k_work_reschedule(&data->touch_detection.touch_end_timeout_work, K_MSEC(config->touch_detection.wait_for_new_position_ms));

    if (event->type != INPUT_EV_ABS && event->type == INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // [3] 좌표 수집 로직
    data->touch_detection.complete = !data->touch_detection.complete;
    if (data->touch_detection.complete && data->touch_detection.absolute != (event->type == INPUT_EV_ABS)) {
        data->touch_detection.absolute = (event->type == INPUT_EV_ABS);
    }

    if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
        data->touch_detection.x = event->value;
    } else if (event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y) {
        data->touch_detection.y = event->value;
    }

    // X축 수집 단계면 여기서 일단 멈춤
    if (! data->touch_detection.complete) {
        data->touch_detection.previous_event = event;
        if (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap) {
            return ZMK_INPUT_PROC_STOP;
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // [4] Y축까지 들어온 시점의 처리
    uint32_t now = k_uptime_get();
    
    // 탭 대기 중일 때는 이전 좌표를 현재 좌표와 일치시켜 delta를 0으로 고정 (커서 튀기 방지 핵심)
    if (data->tap_detection.is_waiting_for_tap) {
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

    // [5] 탭 탈출 조건 (임계값 10유닛으로 상향 - 확실한 고정)
    if (data->tap_detection.is_waiting_for_tap) {
        int dx = data->touch_detection.x - data->touch_detection.previous_x;
        int dy = data->touch_detection.y - data->touch_detection.previous_y;
        if (dx > 10 || dx < -10 || dy > 10 || dy < -10) {
            data->tap_detection.is_waiting_for_tap = false;
        }
    }

    if (!data->touch_detection.touching){
        data->touch_detection.touching = true;
        
        // [기능] ignore_layers 체크 후 오토 레이어 활성화
        if (config->tap_detection.touch_layer >= 0) {
            bool scroller_active = false;
            for (int i = 0; i < config->tap_detection.ignore_layers_len; i++) {
                if (zmk_keymap_layer_active((uint8_t)config->tap_detection.ignore_layers[i])) {
                    scroller_active = true;
                    break;
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

    // [6] 최종 전송 결정
    if (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap) {
        return ZMK_INPUT_PROC_STOP;
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
