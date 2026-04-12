/*
 * tap_detection.c - 통합 수정본
 */

#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include "input_processor_gestures.h"

LOG_MODULE_DECLARE(gestures, CONFIG_ZMK_LOG_LEVEL);

static uint8_t resolve_tap_button(const struct gesture_config *config) {
    int right_click_layer = config->tap_detection.right_click_layer;
    if (right_click_layer >= 0 && zmk_keymap_layer_active((uint8_t)right_click_layer)) {
        return 1; /* right click */
    }
    return 0; /* left click */
}

int tap_detection_handle_start(const struct device *dev, struct gesture_event_t *event) {
    struct gesture_config *config = (struct gesture_config *)dev->config;
    struct gesture_data *data = (struct gesture_data *)dev->data;

    if (! config->tap_detection.enabled) {
        return -1;
    }

    if (data->tap_detection.is_in_drag_window) {
        k_work_cancel_delayable(&data->tap_detection.drag_window_work);
        data->tap_detection.is_in_drag_window = false;
        data->tap_detection.is_dragging = true;
        return 0;
    }

    // 탭 대기 시작
    k_work_reschedule(&data->tap_detection.tap_timeout_work, K_MSEC(config->tap_detection.tap_timout_ms));
    data->tap_detection.is_waiting_for_tap = true;

    // 시작 시점의 이벤트를 0으로 강제 고정
    if (config->tap_detection.prevent_movement_during_tap) {
        event->raw_event_1->type = INPUT_EV_REL;
        event->raw_event_1->value = 0;
        event->raw_event_2->type = INPUT_EV_REL;
        event->raw_event_2->value = 0;
    }

    return 0;
}

int tap_detection_handle_touch(const struct device *dev, struct gesture_event_t *event) {
    struct gesture_config *config = (struct gesture_config *)dev->config;
    struct gesture_data *data = (struct gesture_data *)dev->data;

    if (! config->tap_detection.enabled) {
        return -1;
    }

    if (data->tap_detection.is_waiting_for_tap) {
        /*
         * Threshold 판정: Cirque 패드의 해상도를 고려하여 15~20 정도로 설정하십시오.
         * 너무 낮으면 터치 직후 미세 진동에 탭 모드가 즉시 풀려버립니다.
         */
        int threshold = 15; 

        if (event->delta_x > threshold || event->delta_x < -threshold || 
            event->delta_y > threshold || event->delta_y < -threshold) {
            
            // 임계값 초과 시 탭 대기 해제 및 타이머 중단
            data->tap_detection.is_waiting_for_tap = false;
            k_work_cancel_delayable(&data->tap_detection.tap_timeout_work);
            
        } else if (config->tap_detection.prevent_movement_during_tap) {
            // 임계값 이내일 때는 커서 이동을 완벽히 차단
            event->raw_event_1->type = INPUT_EV_REL;
            event->raw_event_1->value = 0;
            event->raw_event_2->type = INPUT_EV_REL;
            event->raw_event_2->value = 0;
        }
    }

    return 0;
}

int tap_detection_handle_end(const struct device *dev) {
    struct gesture_data *data = (struct gesture_data *)dev->data;
    if (data->tap_detection.is_dragging) {
        zmk_hid_mouse_button_release(data->tap_detection.tap_button);
        zmk_endpoints_send_mouse_report();
        data->tap_detection.is_dragging = false;
    }
    return 0;
}

static void drag_window_timeout_callback(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    struct tap_detection_data *data = CONTAINER_OF(d_work, struct tap_detection_data, drag_window_work);
    if (data->is_in_drag_window) {
        zmk_hid_mouse_button_release(data->tap_button);
        zmk_endpoints_send_mouse_report();
        data->is_in_drag_window = false;
    }
}

static void tap_timeout_callback(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    struct tap_detection_data *data = CONTAINER_OF(d_work, struct tap_detection_data, tap_timeout_work);
    data->is_waiting_for_tap = false;

    if (!data->all->touch_detection.touching) {
        const struct gesture_config *config = (const struct gesture_config *)data->all->dev->config;
        uint8_t button = resolve_tap_button(config);
        zmk_hid_mouse_button_press(button);
        zmk_endpoints_send_mouse_report();

        if (config->tap_detection.tap_to_drag) {
            data->tap_button = button;
            data->is_in_drag_window = true;
            k_work_reschedule(&data->drag_window_work, K_MSEC(config->tap_detection.tap_drag_window_ms));
        } else {
            zmk_hid_mouse_button_release(button);
            zmk_endpoints_send_mouse_report();
        }
    }
}

int tap_detection_init(const struct device *dev) {
    struct gesture_config *config = (struct gesture_config *)dev->config;
    struct gesture_data *data = (struct gesture_data *)dev->data;
    if (!config->tap_detection.enabled) { return -1; }
    k_work_init_delayable(&data->tap_detection.tap_timeout_work, tap_timeout_callback);
    k_work_init_delayable(&data->tap_detection.drag_window_work, drag_window_timeout_callback);
    return 0;
}
