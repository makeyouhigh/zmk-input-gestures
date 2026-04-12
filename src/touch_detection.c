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
    
    // □ [1] 좌표 신호 판별 (X, Y 축 신호 필터링)
    bool is_coord = (event->code == INPUT_ABS_X || event->code == INPUT_REL_X || 
                     event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y);

    if (!is_coord) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    k_work_reschedule(&data->touch_detection.touch_end_timeout_work, K_MSEC(config->touch_detection.wait_for_new_position_ms));

    // □ [2] 원본 데이터 수집 (변조 전의 순수 좌표를 먼저 저장)
    if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
        data->touch_detection.x = event->value;
    } else if (event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y) {
        data->touch_detection.y = event->value;
    }

    data->touch_detection.complete = !data->touch_detection.complete;

    // □ [3] 제스처 로직 처리 (내부 계산에는 원본 좌표 사용)
    if (data->touch_detection.complete) {
        uint32_t now = k_uptime_get();
        
        // 터치 시작 시 기준점 동기화 (커서 점프 방지)
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
        };

        data->touch_detection.last_touch_timestamp = now;

        // 탭 대기 락 해제 체크 (임계값 10)
        if (data->tap_detection.is_waiting_for_tap && 
           (gesture_event.delta_x > 10 || gesture_event.delta_x < -10 || 
            gesture_event.delta_y > 10 || gesture_event.delta_y < -10)) {
            data->tap_detection.is_waiting_for_tap = false;
        }

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
            // [핵심] 여기서 써클 스크롤 신호를 원본 좌표로 계산합니다.
            config->handle_touch_continue(dev, &gesture_event);
        }

        data->touch_detection.previous_x = data->touch_detection.x;
        data->touch_detection.previous_y = data->touch_detection.y;
    }

    // □ [4] 최종 출력 변조 (컴퓨터로 나가는 신호만 속이기)
    // - 탭 대기 중일 때만 마우스 커서를 고정합니다.
    // - 써클 스크롤 중일 때는 드라이버 자체 로직이 신호를 처리하도록 방해하지 않습니다.
    if (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap) {
        event->type = INPUT_EV_REL; // 상대 좌표로 강제 변환
        event->value = 0;           // 이동량 0으로 고정
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
