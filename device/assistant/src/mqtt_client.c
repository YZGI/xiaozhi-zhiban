#include "mqtt_client.h"
#include "plog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/prctl.h>
#include <sys/time.h>

#define MQTT_PACKET_CONNECT     1
#define MQTT_PACKET_CONNACK     2
#define MQTT_PACKET_PUBLISH     3
#define MQTT_PACKET_PUBACK      4
#define MQTT_PACKET_SUBSCRIBE   8
#define MQTT_PACKET_SUBACK      9
#define MQTT_PACKET_PINGREQ     12
#define MQTT_PACKET_PINGRESP    13
#define MQTT_PACKET_DISCONNECT  14

#define MQTT_CONNECT_CLEAN_SESSION 0x02
#define MQTT_CONNECT_USERNAME_FLAG 0x80
#define MQTT_CONNECT_PASSWORD_FLAG 0x40

static int encode_remaining_length(uint8_t *buf, int length)
{
    int idx = 0;
    do {
        uint8_t byte = length % 128;
        length /= 128;
        if (length > 0)
            byte |= 0x80;
        buf[idx++] = byte;
    } while (length > 0);
    return idx;
}

static int decode_remaining_length(const uint8_t *buf, int *length)
{
    int multiplier = 1;
    int idx = 0;
    *length = 0;
    do {
        if (idx >= 4)
            return -1;
        *length += (buf[idx] & 0x7F) * multiplier;
        multiplier *= 128;
    } while (buf[idx++] & 0x80);
    return idx;
}

static int write_utf8_string(uint8_t *buf, const char *str)
{
    int len = strlen(str);
    buf[0] = (len >> 8) & 0xFF;
    buf[1] = len & 0xFF;
    memcpy(buf + 2, str, len);
    return 2 + len;
}

static int mqtt_send_packet(mqtt_client_t *client, const uint8_t *packet, size_t len)
{
    pthread_mutex_lock(&client->send_mutex);
    int ret = tls_transport_write(&client->tls, packet, len);
    pthread_mutex_unlock(&client->send_mutex);
    if (ret < 0) {
        PLOG_E("MQTT", "发送数据包失败");
        return -1;
    }
    return 0;
}

static int mqtt_read_exact(mqtt_client_t *client, uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int ret = tls_transport_read(&client->tls, buf + total, len - total);
        if (ret <= 0 && ret != -2) {
            return -1;
        }
        if (ret > 0)
            total += ret;
        if (ret == -2) {
            usleep(1000);
        }
    }
    return 0;
}

static int mqtt_read_packet(mqtt_client_t *client, uint8_t *ptype, uint8_t *pflags, uint8_t *payload, int *payload_len)
{
    uint8_t first_byte;
    if (mqtt_read_exact(client, &first_byte, 1) != 0)
        return -1;

    *ptype = (first_byte >> 4) & 0x0F;
    *pflags = first_byte & 0x0F;

    int remaining = 0;
    int multiplier = 1;
    uint8_t byte;
    do {
        if (mqtt_read_exact(client, &byte, 1) != 0)
            return -1;
        remaining += (byte & 0x7F) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128 * 128)
            return -1;
    } while (byte & 0x80);

    if (remaining > MQTT_MAX_PAYLOAD) {
        PLOG_E("MQTT", "数据包过大: %d", remaining);
        uint8_t discard[1024];
        while (remaining > 0) {
            int chunk = remaining > 1024 ? 1024 : remaining;
            if (mqtt_read_exact(client, discard, chunk) != 0)
                return -1;
            remaining -= chunk;
        }
        return 0;
    }

    if (remaining > 0) {
        if (mqtt_read_exact(client, payload, remaining) != 0)
            return -1;
    }
    *payload_len = remaining;
    return 0;
}

static int send_connect(mqtt_client_t *client)
{
    int client_id_len = strlen(client->client_id);
    int username_len = client->username[0] ? strlen(client->username) : 0;
    int password_len = client->password[0] ? strlen(client->password) : 0;

    int var_header_len = 10 + 2 + client_id_len;
    int connect_flags = MQTT_CONNECT_CLEAN_SESSION;
    if (username_len > 0) {
        var_header_len += 2 + username_len;
        connect_flags |= MQTT_CONNECT_USERNAME_FLAG;
    }
    if (password_len > 0) {
        var_header_len += 2 + password_len;
        connect_flags |= MQTT_CONNECT_PASSWORD_FLAG;
    }

    int buf_size = 1 + 4 + var_header_len;
    uint8_t *packet = (uint8_t *)malloc(buf_size);
    if (!packet) {
        PLOG_E("MQTT", "CONNECT内存分配失败: %d", buf_size);
        return -1;
    }

    int pos = 0;
    packet[pos++] = (MQTT_PACKET_CONNECT << 4);
    pos += encode_remaining_length(packet + pos, var_header_len);

    packet[pos++] = 0x00;
    packet[pos++] = 0x04;
    packet[pos++] = 'M';
    packet[pos++] = 'Q';
    packet[pos++] = 'T';
    packet[pos++] = 'T';
    packet[pos++] = 4;
    packet[pos++] = connect_flags;
    packet[pos++] = (client->keepalive >> 8) & 0xFF;
    packet[pos++] = client->keepalive & 0xFF;

    pos += write_utf8_string(packet + pos, client->client_id);

    if (connect_flags & MQTT_CONNECT_USERNAME_FLAG)
        pos += write_utf8_string(packet + pos, client->username);
    if (connect_flags & MQTT_CONNECT_PASSWORD_FLAG)
        pos += write_utf8_string(packet + pos, client->password);

    PLOG_I("MQTT", "发送CONNECT (client_id=%s, keepalive=%d, packet_size=%d)", client->client_id, client->keepalive, pos);
    int ret = mqtt_send_packet(client, packet, pos);
    free(packet);
    return ret;
}

static int send_subscribe(mqtt_client_t *client, const char *topic, uint16_t packet_id)
{
    uint8_t packet[512];
    int pos = 0;

    packet[pos++] = (MQTT_PACKET_SUBSCRIBE << 4) | 0x02;

    int var_header_len = 2 + 2 + strlen(topic) + 1;
    pos += encode_remaining_length(packet + pos, var_header_len);

    packet[pos++] = (packet_id >> 8) & 0xFF;
    packet[pos++] = packet_id & 0xFF;

    pos += write_utf8_string(packet + pos, topic);

    packet[pos++] = 0x00;

    PLOG_I("MQTT", "发送SUBSCRIBE (topic=%s, id=%d)", topic, packet_id);
    return mqtt_send_packet(client, packet, pos);
}

static int send_pingreq(mqtt_client_t *client)
{
    uint8_t packet[2];
    packet[0] = (MQTT_PACKET_PINGREQ << 4);
    packet[1] = 0;
    return mqtt_send_packet(client, packet, 2);
}

static int send_disconnect(mqtt_client_t *client)
{
    uint8_t packet[2];
    packet[0] = (MQTT_PACKET_DISCONNECT << 4);
    packet[1] = 0;
    return mqtt_send_packet(client, packet, 2);
}

static int send_puback(mqtt_client_t *client, uint16_t packet_id)
{
    uint8_t packet[4];
    packet[0] = (MQTT_PACKET_PUBACK << 4);
    packet[1] = 2;
    packet[2] = (packet_id >> 8) & 0xFF;
    packet[3] = packet_id & 0xFF;
    PLOG_D("MQTT", "发送PUBACK (id=%d)", packet_id);
    return mqtt_send_packet(client, packet, 4);
}

static void handle_publish(mqtt_client_t *client, uint8_t flags, uint8_t *payload, int payload_len)
{
    if (payload_len < 2)
        return;

    int topic_len = (payload[0] << 8) | payload[1];
    if (2 + topic_len > payload_len)
        return;

    int orig_topic_len = topic_len;
    char topic[MQTT_MAX_TOPIC];
    if (topic_len >= MQTT_MAX_TOPIC)
        topic_len = MQTT_MAX_TOPIC - 1;
    memcpy(topic, payload + 2, topic_len);
    topic[topic_len] = '\0';

    int data_offset = 2 + orig_topic_len;

    int qos = (flags >> 1) & 0x03;
    if (qos > 0)
    {
        if (data_offset + 2 > payload_len)
            return;
        uint16_t packet_id = (payload[data_offset] << 8) | payload[data_offset + 1];
        data_offset += 2;

        if (qos == 1)
        {
            send_puback(client, packet_id);
        }
    }

    int data_len = payload_len - data_offset;

    if (client->on_message && data_len > 0) {
        char *msg = (char *)(payload + data_offset);
        client->on_message(topic, msg, data_len, client->user_data);
    }
}

static void *recv_thread_func(void *arg)
{
    mqtt_client_t *client = (mqtt_client_t *)arg;
    prctl(PR_SET_NAME, "mqtt_recv");

    PLOG_I("MQTT", "接收线程已启动");

    uint64_t last_ping_ms = 0;
    uint8_t payload[MQTT_MAX_PAYLOAD];

    while (client->running && client->connected) {
        int poll_ret = tls_transport_poll(&client->tls, 1000);
        if (poll_ret < 0) {
            PLOG_E("MQTT", "连接断开 (poll错误)");
            break;
        }

        if (poll_ret == 0) {
            uint64_t now = 0;
            struct timeval tv;
            gettimeofday(&tv, NULL);
            now = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

            if (client->keepalive > 0 && last_ping_ms > 0 &&
                (now - last_ping_ms) > (uint64_t)client->keepalive * 1000 * 3 / 4) {
                if (send_pingreq(client) == 0) {
                    PLOG_D("MQTT", "发送PINGREQ");
                    last_ping_ms = now;
                }
            }
            continue;
        }

        uint8_t ptype;
        uint8_t pflags;
        int plen;
        if (mqtt_read_packet(client, &ptype, &pflags, payload, &plen) != 0) {
            PLOG_E("MQTT", "读取数据包失败");
            break;
        }

        struct timeval tv;
        gettimeofday(&tv, NULL);
        last_ping_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

        switch (ptype) {
        case MQTT_PACKET_CONNACK:
            PLOG_D("MQTT", "收到CONNACK (code=%d)", plen > 1 ? payload[1] : -1);
            break;

        case MQTT_PACKET_PUBLISH:
            handle_publish(client, pflags, payload, plen);
            break;

        case MQTT_PACKET_PUBACK:
            PLOG_D("MQTT", "收到PUBACK");
            break;

        case MQTT_PACKET_SUBACK:
            PLOG_D("MQTT", "收到SUBACK");
            break;

        case MQTT_PACKET_PINGRESP:
            PLOG_D("MQTT", "收到PINGRESP");
            break;

        default:
            PLOG_D("MQTT", "收到未知包类型=%d", ptype);
            break;
        }
    }

    client->connected = false;
    PLOG_I("MQTT", "接收线程已退出");

    if (client->on_disconnected)
        client->on_disconnected(client->user_data);

    return NULL;
}

int mqtt_client_init(mqtt_client_t *client)
{
    if (!client)
        return -1;
    memset(client, 0, sizeof(mqtt_client_t));
    client->keepalive = MQTT_KEEPALIVE_DEFAULT;
    pthread_mutex_init(&client->send_mutex, NULL);
    return 0;
}

void mqtt_client_destroy(mqtt_client_t *client)
{
    if (!client)
        return;
    mqtt_client_disconnect(client);
    pthread_mutex_destroy(&client->send_mutex);
}

void mqtt_client_set_callbacks(mqtt_client_t *client,
                                mqtt_message_cb_t on_message,
                                mqtt_connected_cb_t on_connected,
                                mqtt_disconnected_cb_t on_disconnected,
                                void *user_data)
{
    if (!client)
        return;
    client->on_message = on_message;
    client->on_connected = on_connected;
    client->on_disconnected = on_disconnected;
    client->user_data = user_data;
}

int mqtt_client_connect(mqtt_client_t *client)
{
    if (!client)
        return -1;

    PLOG_I("MQTT", "正在连接 %s:%d", client->host, client->port);

    if (tls_transport_init(&client->tls) != 0) {
        snprintf(client->error, sizeof(client->error), "TLS初始化失败");
        return -1;
    }

    if (tls_transport_connect(&client->tls, client->host, client->port) != 0) {
        snprintf(client->error, sizeof(client->error), "TLS连接失败: %s", tls_transport_get_error(&client->tls));
        tls_transport_destroy(&client->tls);
        return -1;
    }

    if (send_connect(client) != 0) {
        snprintf(client->error, sizeof(client->error), "发送CONNECT失败");
        tls_transport_destroy(&client->tls);
        return -1;
    }

    uint8_t ptype;
    uint8_t pflags;
    uint8_t payload[256];
    int plen;
    if (mqtt_read_packet(client, &ptype, &pflags, payload, &plen) != 0) {
        snprintf(client->error, sizeof(client->error), "等待CONNACK超时");
        tls_transport_destroy(&client->tls);
        return -1;
    }

    if (ptype != MQTT_PACKET_CONNACK) {
        snprintf(client->error, sizeof(client->error), "期望CONNACK, 收到类型=%d", ptype);
        tls_transport_destroy(&client->tls);
        return -1;
    }

    int return_code = (plen >= 2) ? payload[1] : -1;
    if (return_code != 0) {
        snprintf(client->error, sizeof(client->error), "CONNACK返回码=%d", return_code);
        tls_transport_destroy(&client->tls);
        return -1;
    }

    client->connected = true;
    PLOG_I("MQTT", "MQTT连接成功");

    if (client->subscribe_topic[0]) {
        client->next_packet_id++;
        if (mqtt_client_subscribe(client, client->subscribe_topic) != 0) {
            PLOG_W("MQTT", "订阅失败: %s", client->subscribe_topic);
        }
    }

    client->running = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_create(&client->recv_thread, &attr, recv_thread_func, client);
    pthread_attr_destroy(&attr);

    if (client->on_connected)
        client->on_connected(client->user_data);

    return 0;
}

void mqtt_client_disconnect(mqtt_client_t *client)
{
    if (!client)
        return;

    client->running = false;

    if (client->connected) {
        send_disconnect(client);
        client->connected = false;
    }

    if (client->recv_thread) {
        pthread_join(client->recv_thread, NULL);
        client->recv_thread = 0;
    }

    tls_transport_destroy(&client->tls);
    PLOG_I("MQTT", "MQTT已断开");
}

int mqtt_client_subscribe(mqtt_client_t *client, const char *topic)
{
    if (!client || !topic || !client->connected)
        return -1;
    client->next_packet_id++;
    return send_subscribe(client, topic, client->next_packet_id);
}

int mqtt_client_publish(mqtt_client_t *client, const char *topic, const char *payload, size_t payload_len)
{
    if (!client || !topic || !payload || !client->connected)
        return -1;

    int topic_len = strlen(topic);
    int var_header_len = 2 + topic_len;
    int remaining = var_header_len + payload_len;
    int buf_size = 1 + 4 + remaining;

    uint8_t stack_buf[512];
    uint8_t *packet = (buf_size <= (int)sizeof(stack_buf)) ? stack_buf : (uint8_t *)malloc(buf_size);
    if (!packet) {
        PLOG_E("MQTT", "PUBLISH内存分配失败: %d", buf_size);
        return -1;
    }

    int pos = 0;
    packet[pos++] = (MQTT_PACKET_PUBLISH << 4) | 0x00;
    pos += encode_remaining_length(packet + pos, remaining);
    pos += write_utf8_string(packet + pos, topic);
    memcpy(packet + pos, payload, payload_len);
    pos += payload_len;

    int ret = mqtt_send_packet(client, packet, pos);
    if (packet != stack_buf) free(packet);
    return ret;
}

bool mqtt_client_is_connected(mqtt_client_t *client)
{
    if (!client)
        return false;
    return client->connected;
}

const char *mqtt_client_get_error(mqtt_client_t *client)
{
    if (!client)
        return "客户端为空";
    return client->error[0] ? client->error : "无错误";
}
