#ifndef UDP_AUDIO_H
#define UDP_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "mbedtls/aes.h"

#define UDP_AUDIO_MAX_PAYLOAD 4800
#define UDP_AUDIO_NONCE_SIZE 16

typedef void (*udp_audio_recv_cb_t)(const uint8_t *data, size_t len, uint32_t timestamp, void *user_data);

typedef struct {
    int sockfd;
    bool connected;

    uint8_t aes_key[16];
    uint8_t aes_nonce_base[UDP_AUDIO_NONCE_SIZE];
    mbedtls_aes_context send_aes_ctx;

    uint32_t local_sequence;
    uint32_t remote_sequence;

    char server_ip[64];
    int server_port;

    pthread_t recv_thread;
    bool recv_running;
    pthread_mutex_t send_mutex;

    udp_audio_recv_cb_t on_recv;
    void *user_data;
} udp_audio_t;

int udp_audio_init(udp_audio_t *udp);
void udp_audio_destroy(udp_audio_t *udp);

void udp_audio_set_callbacks(udp_audio_t *udp, udp_audio_recv_cb_t on_recv, void *user_data);

int udp_audio_connect(udp_audio_t *udp, const char *server_ip, int server_port,
                       const char *key_hex, const char *nonce_hex);
void udp_audio_disconnect(udp_audio_t *udp);

int udp_audio_send(udp_audio_t *udp, const uint8_t *opus_data, size_t opus_len, uint32_t timestamp);

bool udp_audio_is_connected(udp_audio_t *udp);

#endif
