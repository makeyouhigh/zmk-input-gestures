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
    
    // [1] 좌표 신호 가드: Sync 신호 등은 흐름을 위해 무조건 통과 (버벅임 방지)
    bool is_coord = (event->code == INPUT_ABS_X || event->code == INPUT_REL_X || 
                     event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y);

    if (!is_coord) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    k_work_reschedule(&data->touch_detection.touch_end_timeout_work, K_MSEC(config->touch_detection.wait_for_new_position_ms));

    if (event->type != INPUT_EV_ABS && event->type == INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // [2] 좌표 데이터 수집 (X, Y 한 쌍 맞추기)
    data->touch_detection.complete = !data->touch_detection.complete;
    if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
        data->touch_detection.x = event->value;
    } else if (event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y) {
        data->touch_detection.y = event->value;
    }

    // X축만 들어온 상태라면 Y축을 기다리며 진행
    if (! data->touch_detection.complete) {
        data->touch_detection.previous_event = event;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // [3] 좌표 튀기 방지: 새로운 터치 시작 시 기준점 초기화
    if (!data->touch_detection.touching) {
        data->touch_detection.previous_x = data->touch_detection.x;
        data->touch_detection.previous_y = data->touch_detection.y;
    }

    uint32_t now = k_uptime_get();
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

    // [4] 움직임 탈출 조건 (Threshold 5: 탭할 때는 멈추고 슥 움직이면 즉시 풀림)
    if (data->tap_detection.is_waiting_for_tap && 
       (gesture_event.delta_x > 5 || gesture_event.delta_x < -5 || 
        gesture_event.delta_y > 5 || gesture_event.delta_y < -5)) {
        data->tap_detection.is_waiting_for_tap = false;
    }

    // [5] 제스처 엔진 실행 (써클 스크롤은 여기서 데이터를 읽어 신호를 생성함)
    if (!data->touch_detection.touching){
        data->touch_detection.touching = true;
        // 오토 레이어 활성화
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

    // [6] 최종 출력 필터: 탭 대기 중이거나 써클 스크롤 중일 때 커서만 멈춤
    // 신호를 차단(STOP)하지 않고, 타입만 REL 0으로 변조하여 전송 (버벅임 해결 핵심)
    bool should_suppress = (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap) ||
                           (data->circular_scroll.is_tracking);

    if (should_suppress) {
        // 첫 번째 이벤트(X) 변조
        data->touch_detection.previous_event->type = INPUT_EV_REL;
        data->touch_detection.previous_event->value = 0;
        // 두 번째 이벤트(Y) 변조
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
