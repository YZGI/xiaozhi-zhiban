#ifndef TOUCH_KEY_H
#define TOUCH_KEY_H

#include <stdint.h>
#include <pthread.h>

typedef struct {
    int fd;
    pthread_t thread;
    int thread_created;
    volatile int running;
    volatile int pending_key;
    void (*on_key)(int key_code, void* user_data);
    void* user_data;
    /* 真实屏幕点击(BTN_TOUCH)回调 + 最近坐标（设备原生 app 也走 /dev/input/event2） */
    void (*on_touch)(int x, int y, void* user_data);
    volatile int last_abs_x;
    volatile int last_abs_y;
    /* 最近一次 GOODIX 软键(HOME/BACK)时间戳(ms, CLOCK_MONOTONIC)，
       用于去抖：软键区点击常同时上报 BTN_TOUCH，避免重复触发。 */
    long long last_key_ms;
} touch_key_t;

int touch_key_init(touch_key_t* tk,
                   void (*on_key)(int key_code, void* user_data),
                   void* user_data);
void touch_key_destroy(touch_key_t* tk);

#endif
