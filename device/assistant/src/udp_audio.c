#include "udp_audio.h"
#include "plog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <netdb.h>
#include <sys/prctl.h>

#include "mbedtls/aes.h"

static int hex_to_bytes(const char *hex, uint8_t *out, int max_len)
{
    int len = strlen(hex);
    if (len % 2 != 0)
        return -1;
    int byte_count = len / 2;
    if (byte_count > max_len)
        byte_count = max_len;
    for (int i = 0; i < byte_count; i++) {
        unsigned int val;
        if (sscanf(hex + i * 2, "%02x", &val) != 1)
            return -1;
        out[i] = (uint8_t)val;
    }
    return byte_count;
}

static void *udp_recv_thread_func(void *arg)
{
    udp_audio_t *udp = (udp_audio_t *)arg;
    prctl(PR_SET_NAME, "udp_recv");

    uint8_t buf[UDP_AUDIO_MAX_PAYLOAD + UDP_AUDIO_NONCE_SIZE + 64];
    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_enc(&aes_ctx, udp->aes_key, 128);

    PLOG_I("UDP", "UDP接收线程已启动 (key[0]=0x%02x)", udp->aes_key[0]);

    int recv_count = 0;

    while (udp->recv_running) {
        struct pollfd pfd;
        pfd.fd = udp->sockfd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 500);
        if (ret <= 0)
            continue;

        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(udp->sockfd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n <= 0)
            continue;

        if (n < UDP_AUDIO_NONCE_SIZE) {
            PLOG_W("UDP", "数据包过短: %zd", n);
            continue;
        }

        if (buf[0] != 0x01) {
            PLOG_W("UDP", "无效数据包类型: 0x%02x", buf[0]);
            continue;
        }

        uint32_t timestamp = ntohl(*(uint32_t *)&buf[8]);
        uint32_t sequence = ntohl(*(uint32_t *)&buf[12]);

        if (sequence < udp->remote_sequence) {
            PLOG_D("UDP", "旧序号包: %u < %u", sequence, udp->remote_sequence);
            continue;
        }
        udp->remote_sequence = sequence;

        size_t encrypted_len = n - UDP_AUDIO_NONCE_SIZE;
        if (encrypted_len == 0)
            continue;

        uint8_t *encrypted = buf + UDP_AUDIO_NONCE_SIZE;
        uint8_t decrypted[UDP_AUDIO_MAX_PAYLOAD];

        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        int aes_ret = mbedtls_aes_crypt_ctr(&aes_ctx, encrypted_len, &nc_off,
                                             buf, stream_block,
                                             encrypted, decrypted);
        if (aes_ret != 0) {
            PLOG_E("UDP", "AES解密失败: %d", aes_ret);
            continue;
        }

        if (udp->on_recv) {
            recv_count++;
            if (recv_count <= 5 || recv_count % 100 == 0) {
                PLOG_I("UDP", "收到音频包 #%d: 解密后=%zu字节 ts=%u seq=%u",
                       recv_count, encrypted_len, timestamp, sequence);
            }
            udp->on_recv(decrypted, encrypted_len, timestamp, udp->user_data);
        }
    }

    mbedtls_aes_free(&aes_ctx);
    PLOG_I("UDP", "UDP接收线程已退出");
    return NULL;
}

int udp_audio_init(udp_audio_t *udp)
{
    if (!udp)
        return -1;
    memset(udp, 0, sizeof(udp_audio_t));
    udp->sockfd = -1;
    pthread_mutex_init(&udp->send_mutex, NULL);
    return 0;
}

void udp_audio_destroy(udp_audio_t *udp)
{
    if (!udp)
        return;
    udp_audio_disconnect(udp);
    pthread_mutex_destroy(&udp->send_mutex);
}

void udp_audio_set_callbacks(udp_audio_t *udp, udp_audio_recv_cb_t on_recv, void *user_data)
{
    if (!udp)
        return;
    udp->on_recv = on_recv;
    udp->user_data = user_data;
}

int udp_audio_connect(udp_audio_t *udp, const char *server_ip, int server_port,
                       const char *key_hex, const char *nonce_hex)
{
    if (!udp || !server_ip || !key_hex || !nonce_hex)
        return -1;

    int key_len = hex_to_bytes(key_hex, udp->aes_key, sizeof(udp->aes_key));
    if (key_len != 16) {
        PLOG_E("UDP", "AES密钥解析失败 (需要32字符hex, 得到%d字节)", key_len);
        return -1;
    }

    int nonce_len = hex_to_bytes(nonce_hex, udp->aes_nonce_base, sizeof(udp->aes_nonce_base));
    if (nonce_len != 16) {
        PLOG_E("UDP", "Nonce解析失败 (需要32字符hex, 得到%d字节)", nonce_len);
        return -1;
    }

    strncpy(udp->server_ip, server_ip, sizeof(udp->server_ip) - 1);
    udp->server_port = server_port;

    udp->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp->sockfd < 0) {
        PLOG_E("UDP", "创建UDP套接字失败: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);

    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(server_ip, NULL, &hints, &result) == 0 && result) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)result->ai_addr;
        memcpy(&serv_addr.sin_addr, &ipv4->sin_addr, sizeof(struct in_addr));
        freeaddrinfo(result);
    } else {
        if (inet_aton(server_ip, &serv_addr.sin_addr) == 0) {
            PLOG_E("UDP", "无法解析地址: %s", server_ip);
            close(udp->sockfd);
            udp->sockfd = -1;
            return -1;
        }
    }

    if (connect(udp->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
        PLOG_E("UDP", "UDP connect失败: %s", strerror(errno));
        close(udp->sockfd);
        udp->sockfd = -1;
        return -1;
    }

    udp->local_sequence = 0;
    udp->remote_sequence = 0;
    udp->connected = true;

    mbedtls_aes_init(&udp->send_aes_ctx);
    mbedtls_aes_setkey_enc(&udp->send_aes_ctx, udp->aes_key, 128);

    udp->recv_running = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    pthread_create(&udp->recv_thread, &attr, udp_recv_thread_func, udp);
    pthread_attr_destroy(&attr);

    PLOG_I("UDP", "已连接到 %s:%d", server_ip, server_port);
    return 0;
}

void udp_audio_disconnect(udp_audio_t *udp)
{
    if (!udp)
        return;

    udp->recv_running = false;
    udp->connected = false;

    if (udp->recv_thread) {
        pthread_join(udp->recv_thread, NULL);
        udp->recv_thread = 0;
    }

    mbedtls_aes_free(&udp->send_aes_ctx);

    if (udp->sockfd >= 0) {
        close(udp->sockfd);
        udp->sockfd = -1;
    }

    PLOG_I("UDP", "UDP已断开");
}

int udp_audio_send(udp_audio_t *udp, const uint8_t *opus_data, size_t opus_len, uint32_t timestamp)
{
    if (!udp || !opus_data || opus_len == 0 || !udp->connected)
        return -1;

    pthread_mutex_lock(&udp->send_mutex);

    uint8_t nonce[UDP_AUDIO_NONCE_SIZE];
    memcpy(nonce, udp->aes_nonce_base, UDP_AUDIO_NONCE_SIZE);
    *(uint16_t *)&nonce[2] = htons((uint16_t)opus_len);
    *(uint32_t *)&nonce[8] = htonl(timestamp);
    *(uint32_t *)&nonce[12] = htonl(++udp->local_sequence);

    uint8_t encrypted[UDP_AUDIO_MAX_PAYLOAD];
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    int ret = mbedtls_aes_crypt_ctr(&udp->send_aes_ctx, opus_len, &nc_off,
                                     nonce, stream_block,
                                     opus_data, encrypted);

    if (ret != 0) {
        pthread_mutex_unlock(&udp->send_mutex);
        PLOG_E("UDP", "AES加密失败: %d", ret);
        return -1;
    }

    uint8_t packet[UDP_AUDIO_NONCE_SIZE + UDP_AUDIO_MAX_PAYLOAD];
    memcpy(packet, nonce, UDP_AUDIO_NONCE_SIZE);
    memcpy(packet + UDP_AUDIO_NONCE_SIZE, encrypted, opus_len);

    ssize_t sent = send(udp->sockfd, packet, UDP_AUDIO_NONCE_SIZE + opus_len, 0);
    pthread_mutex_unlock(&udp->send_mutex);

    if (sent < 0) {
        PLOG_E("UDP", "发送失败: %s", strerror(errno));
        return -1;
    }

    return 0;
}

bool udp_audio_is_connected(udp_audio_t *udp)
{
    if (!udp)
        return false;
    return udp->connected;
}
