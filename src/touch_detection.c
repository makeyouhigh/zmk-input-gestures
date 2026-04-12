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
    
    // □ [1] 좌표 신호 판별
    // - Sync 신호 등은 흐름을 방해하지 않도록 무조건 통과시킵니다.
    bool is_coord = (event->code == INPUT_ABS_X || event->code == INPUT_REL_X || 
                     event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y);

    if (!is_coord) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    k_work_reschedule(&data->touch_detection.touch_end_timeout_work, K_MSEC(config->touch_detection.wait_for_new_position_ms));

    if (event->type != INPUT_EV_ABS && event->type == INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // □ [2] 좌표 데이터 수집 (X, Y 페어 구성)
    data->touch_detection.complete = !data->touch_detection.complete;
    if (event->code == INPUT_ABS_X || event->code == INPUT_REL_X) {
        data->touch_detection.x = event->value;
    } else if (event->code == INPUT_ABS_Y || event->code == INPUT_REL_Y) {
        data->touch_detection.y = event->value;
    }

    // □ [3] 이동 억제 조건 설정
    // - 탭 대기 중이거나 써클 투 스크롤 작동 중에 활성화됩니다.
    bool should_suppress = (data->tap_detection.is_waiting_for_tap && config->tap_detection.prevent_movement_during_tap) ||
                           (data->circular_scroll.is_tracking);

    // X축만 수집된 상태일 때의 처리
    if (! data->touch_detection.complete) {
        data->touch_detection.previous_event = event;
        // 억제 조건 시 신호를 REL 0으로 변조하여 전송 (시스템 렉 방지)
        if (should_suppress) {
            event->type = INPUT_EV_REL; event->value = 0;
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // □ [4] Y축까지 포함된 완전한 좌표 처리 시작
    uint32_t now = k_uptime_get();
    
    // 새로운 터치 시작 시 좌표 동기화 (커서 순간 이동 방지)
    if (!data->touch_detection.touching) {
        data->touch_detection.previous_x = data->touch_detection.x;
        data->touch_detection.previous_y = data->touch_detection.y;
    }

    struct gesture_event_t gesture_event = {
        .last_touch_timestamp = now,
        .previous_touch_timestamp = data->touch_detection.
