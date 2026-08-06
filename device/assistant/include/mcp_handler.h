#ifndef MCP_HANDLER_H
#define MCP_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef int (*mcp_send_json_cb_t)(const char *json, size_t len, void *user_data);

typedef struct {
    void *lib_handle;

    int (*sound_set_sys_volume)(int volume);
    int (*sound_get_sys_volume)(void);
    int (*sound_set_sys_mute)(int mute);
    int (*sound_is_sys_mute)(void);
    int (*power_get_charge_status)(int *out);
    int (*power_get_battery_cap)(void);
    int (*power_get_battery_voltage)(void);
    void (*sound_tts_play)(int index);

    /* 视频播放库 (libsmart_player_api.so, 工厂硬件解码) */
    void *player_handle;
    int (*splayer_open)(void);
    int (*splayer_close)(void);
    int (*splayer_stop)(void);
    int (*splayer_set_file)(const char *url);
    int (*splayer_play)(void);
    int (*splayer_set_volume)(int vol);

    /* 流媒体核心播放库 (libmusic_player_api.so)。这是学习软件/原厂播放器
       smart_player 真正用来播在线流的接口（mp_open 自启播放器服务并返回句柄，
       mp_set_file 设流地址，mp_play 出画），支持 http/hls/rtsp 拉流。
       splayer_* 只是其本地文件型封装，播 http 流会走错分支（实测 msg->type:0）。
       看电视主路径用 mp_*，splayer_* 仅作兜底。 */
    void *music_handle;
    void *(*mp_open)(void);
    int (*mp_close)(void *);
    int (*mp_set_file)(void *, const char *);
    int (*mp_play)(void *);
    int (*mp_stop)(void *);
    int (*mp_pause)(void *);
    int (*mp_resume)(void *);
    int (*mp_set_volume)(void *, int);

    /* 设备原厂媒体导航库 (libmedia_navi_api.so, 走 olmedia_service 云目录，
       仅能播厂商云目录条目，不能播任意 URL，已弃用为播放主路径) */
    void *navi_handle;
    int (*media_navi_open)(const char *url);
    int (*media_navi_close)(void);

    mcp_send_json_cb_t send_json;
    void *user_data;
} mcp_handler_t;

int mcp_handler_init(mcp_handler_t *mcp);
void mcp_handler_destroy(mcp_handler_t *mcp);
void mcp_handler_set_send_cb(mcp_handler_t *mcp, mcp_send_json_cb_t send_cb, void *user_data);
void mcp_handler_process_message(mcp_handler_t *mcp, const char *json, size_t len);

/* 看电视：离线命令词触发，绕过云端对话（设备原厂媒体链路） */
int mcp_play_tv(mcp_handler_t *mcp, const char *url);
int mcp_stop_tv(mcp_handler_t *mcp);
int mcp_tv_is_playing(void);

#endif
