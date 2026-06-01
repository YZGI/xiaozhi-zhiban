#ifndef AUDIO_RECORDER_MODULE_H
#define AUDIO_RECORDER_MODULE_H

#include "audio_dispatcher.h"
#include "protocol_handler.h"
#include "audio_precache.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define RECORDER_FRAME_SIZE 960
#define RECORDER_OPUS_BUF_SIZE 1500
#define RECORDER_SAMPLE_RATE 16000
#define RECORDER_CHANNELS 1
#define RECORDER_BITRATE 16000
#define RECORDER_FRAME_DURATION 60

typedef struct {
    protocol_handler_t *proto;
    audio_dispatcher_t *disp;

    int16_t mic_sample_buf[RECORDER_FRAME_SIZE];
    int frame_count;

    void *opus_encoder;
    uint8_t opus_output_buf[RECORDER_OPUS_BUF_SIZE];

    volatile bool sending;
    audio_precache_t precache;
    pthread_mutex_t mutex;
} audio_recorder_module_t;

int audio_recorder_module_init(audio_recorder_module_t *rec, protocol_handler_t *proto, audio_dispatcher_t *disp);
int audio_recorder_module_early_init(audio_recorder_module_t *rec, audio_dispatcher_t *disp);
void audio_recorder_module_set_proto(audio_recorder_module_t *rec, protocol_handler_t *proto);
void audio_recorder_module_destroy(audio_recorder_module_t *rec);

int audio_recorder_module_start_sending(audio_recorder_module_t *rec);
int audio_recorder_module_stop_sending(audio_recorder_module_t *rec);
bool audio_recorder_module_is_sending(audio_recorder_module_t *rec);

#endif
