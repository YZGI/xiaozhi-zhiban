#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "tls_transport.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define MQTT_MAX_CLIENT_ID 256
#define MQTT_MAX_TOPIC 256
#define MQTT_MAX_PAYLOAD 4096
#define MQTT_MAX_HOST 128
#define MQTT_KEEPALIVE_DEFAULT 240

typedef void (*mqtt_message_cb_t)(const char *topic, const char *payload, size_t payload_len, void *user_data);
typedef void (*mqtt_connected_cb_t)(void *user_data);
typedef void (*mqtt_disconnected_cb_t)(void *user_data);

typedef struct {
    char host[MQTT_MAX_HOST];
    int port;
    char client_id[MQTT_MAX_CLIENT_ID];
    char username[512];
    char password[512];
    int keepalive;
    char subscribe_topic[MQTT_MAX_TOPIC];
    char publish_topic[MQTT_MAX_TOPIC];

    tls_transport_t tls;
    bool connected;
    bool running;
    pthread_t recv_thread;
    pthread_mutex_t send_mutex;

    uint16_t next_packet_id;

    mqtt_message_cb_t on_message;
    mqtt_connected_cb_t on_connected;
    mqtt_disconnected_cb_t on_disconnected;
    void *user_data;

    char error[256];
} mqtt_client_t;

int mqtt_client_init(mqtt_client_t *client);
void mqtt_client_destroy(mqtt_client_t *client);

void mqtt_client_set_callbacks(mqtt_client_t *client,
                                mqtt_message_cb_t on_message,
                                mqtt_connected_cb_t on_connected,
                                mqtt_disconnected_cb_t on_disconnected,
                                void *user_data);

int mqtt_client_connect(mqtt_client_t *client);
void mqtt_client_disconnect(mqtt_client_t *client);

int mqtt_client_subscribe(mqtt_client_t *client, const char *topic);
int mqtt_client_publish(mqtt_client_t *client, const char *topic, const char *payload, size_t payload_len);

bool mqtt_client_is_connected(mqtt_client_t *client);
const char *mqtt_client_get_error(mqtt_client_t *client);

#endif
