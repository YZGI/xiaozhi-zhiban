#include "audio_recorder.h"
#include "plog.h"
#include <string.h>
#include <stdlib.h>

#include "opus.h"

static void do_encode_send(audio_recorder_module_t *rec)
{
    int opus_len = opus_encode((OpusEncoder *)rec->opus_encoder,
                               rec->mic_sample_buf,
                               rec->frame_count,
                               rec->opus_output_buf,
                               RECORDER_OPUS_BUF_SIZE);

    rec->frame_count = 0;

    if (opus_len > 0)
    {
        if (rec->sending)
        {
            protocol_handler_send_audio(rec->proto, rec->opus_output_buf, opus_len);
        }
        else if (audio_precache_is_active(&rec->precache))
        {
            audio_precache_push(&rec->precache, rec->opus_output_buf, opus_len);
        }
    }
    else if (opus_len < 0)
    {
        PLOG_W("REC", "opus_encode 编码失败: %d", opus_len);
    }
}

static void on_audio_data(const int16_t *data, int len, void *user_data)
{
    audio_recorder_module_t *rec = (audio_recorder_module_t *)user_data;
    if (!rec)
        return;

    if (!rec->sending && !audio_precache_is_active(&rec->precache))
        return;

    pthread_mutex_lock(&rec->mutex);

    int total_samples = len / 3;

    for (int i = 0; i < total_samples; i++)
    {
        rec->mic_sample_buf[rec->frame_count] = data[i * 3 + 1];
        rec->frame_count++;

        if (rec->frame_count >= RECORDER_FRAME_SIZE)
        {
            do_encode_send(rec);
        }
    }

    pthread_mutex_unlock(&rec->mutex);
}

static int create_opus_encoder(audio_recorder_module_t *rec)
{
    int error;
    if (rec->opus_encoder)
    {
        opus_encoder_destroy((OpusEncoder *)rec->opus_encoder);
        rec->opus_encoder = NULL;
    }
    rec->opus_encoder = opus_encoder_create(RECORDER_SAMPLE_RATE, RECORDER_CHANNELS,
                                            OPUS_APPLICATION_VOIP, &error);
    if (!rec->opus_encoder || error != OPUS_OK)
    {
        PLOG_E("REC", "opus_encoder_create 创建失败: %d (%s)", error, opus_strerror(error));
        return -1;
    }
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_BITRATE(RECORDER_BITRATE));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl((OpusEncoder *)rec->opus_encoder, OPUS_SET_INBAND_FEC(0));
    return 0;
}

static int recorder_init_common(audio_recorder_module_t *rec, protocol_handler_t *proto, audio_dispatcher_t *disp)
{
    memset(rec, 0, sizeof(audio_recorder_module_t));
    rec->proto = proto;
    rec->disp = disp;

    if (create_opus_encoder(rec) != 0)
        return -1;

    pthread_mutex_init(&rec->mutex, NULL);
    audio_precache_init(&rec->precache);

    audio_dispatcher_register(disp, on_audio_data, rec);

    PLOG_I("REC", "初始化完成: 采样率=%d 声道=%d 码率=%d%s",
           RECORDER_SAMPLE_RATE, RECORDER_CHANNELS, RECORDER_BITRATE,
           proto ? "" : " (无proto)");
    return 0;
}

int audio_recorder_module_init(audio_recorder_module_t *rec, protocol_handler_t *proto, audio_dispatcher_t *disp)
{
    if (!rec || !proto || !disp)
        return -1;
    return recorder_init_common(rec, proto, disp);
}

int audio_recorder_module_early_init(audio_recorder_module_t *rec, audio_dispatcher_t *disp)
{
    if (!rec || !disp)
        return -1;
    return recorder_init_common(rec, NULL, disp);
}

void audio_recorder_module_set_proto(audio_recorder_module_t *rec, protocol_handler_t *proto)
{
    if (!rec || !proto)
        return;
    rec->proto = proto;
    PLOG_I("REC", "proto 已关联");
}

void audio_recorder_module_destroy(audio_recorder_module_t *rec)
{
    if (!rec)
        return;

    audio_recorder_module_stop_sending(rec);

    audio_precache_stop(&rec->precache);

    audio_dispatcher_unregister(rec->disp, on_audio_data);

    if (rec->opus_encoder)
    {
        opus_encoder_destroy((OpusEncoder *)rec->opus_encoder);
        rec->opus_encoder = NULL;
    }

    audio_precache_destroy(&rec->precache);
    pthread_mutex_destroy(&rec->mutex);
}

int audio_recorder_module_start_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return -1;

    pthread_mutex_lock(&rec->mutex);
    rec->frame_count = 0;
    rec->sending = true;
    pthread_mutex_unlock(&rec->mutex);

    PLOG_I("REC", "已开始发送录音");
    return 0;
}

int audio_recorder_module_stop_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return -1;

    pthread_mutex_lock(&rec->mutex);
    rec->sending = false;
    rec->frame_count = 0;
    pthread_mutex_unlock(&rec->mutex);

    PLOG_I("REC", "已停止发送录音");
    return 0;
}

bool audio_recorder_module_is_sending(audio_recorder_module_t *rec)
{
    if (!rec)
        return false;
    return rec->sending;
}
