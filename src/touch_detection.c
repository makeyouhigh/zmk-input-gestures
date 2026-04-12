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

    // Sync 신호 등은 시스템 흐름을 위해 무조건 통과 (버벅임 방지)
    if (!is_coord) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    k_work_reschedule(&data->touch_detection.touch_end_timeout_work, K_MSEC(config->touch_detection.wait_for_new_position_ms));

    // □ [2] 원본 데이터 보관 및 출력 신호 변조 준비
    // - 탭 대기 중이거나 써클 스크롤 작동 중이면 커서 고정 모드 활성화
    bool should_suppress = (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap) ||
                           (data->circular_scroll.is_tracking);

    if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
        data->touch_detection.x = event->value;
    } else if (event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y) {
        data->touch_detection.y = event->value;
    }

    // □ [3] 핵심: 신호를 차단(STOP)하지 않고 '상대 좌표 0'으로 속여서 전송
    // - 이렇게 해야 시스템 렉이 없고 써클 스크롤이 좌표를 읽을 수 있습니다.
    if (should_suppress) {
        event->type = INPUT_EV_REL; // 강제 상대 좌표 변환
        event->value = 0;           // 이동량 0으로 고정
    }

    data->touch_detection.complete = !data->touch_detection.complete;

    // □ [4] Y축까지 들어온 한 세트의 패킷 처리
    if (data->touch_detection.complete) {
        uint32_t now = k_uptime_get();
        
        // 터치 시작 시 좌표 동기화 (커서 점프 현상 원천 차단)
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

        // 탭 락 해제 임계값 (5유닛 이상 의도적으로 움직이면 락 해제)
        if (data->tap_detection.is_waiting_for_tap && 
           (gesture_event.delta_x > 5 || gesture_event.delta_x < -5 || 
            gesture_event.delta_y > 5 || gesture_event.delta_y < -5)) {
            data->tap_detection.is_waiting_for_tap = false;
        }

        // 제스처 엔진 실행 (써클 스크롤은 여기서 좌표를 읽어 스크롤 신호를 생성함)
        if (!data->touch_detection.touching){
            data->touch_detection.touching = true;
            // 오토 레이어 활성화 체크
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
        zmk_keymap_layer
