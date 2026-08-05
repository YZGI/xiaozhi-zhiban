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

    mcp_send_json_cb_t send_json;
    void *user_data;
} mcp_handler_t;

int mcp_handler_init(mcp_handler_t *mcp);
void mcp_handler_destroy(mcp_handler_t *mcp);
void mcp_handler_set_send_cb(mcp_handler_t *mcp, mcp_send_json_cb_t send_cb, void *user_data);
void mcp_handler_process_message(mcp_handler_t *mcp, const char *json, size_t len);

/* 本地电视指令：判断用户文本 1=播放 2=停止 0=无关（绕开 AI function calling） */
int tv_cmd_type(const char *text);
/* 播放电视，url 为 NULL 时使用默认源 TV_DEFAULT_URL */
int mcp_play_tv(mcp_handler_t *mcp, const char *url);
/* 停止电视播放 */
int mcp_stop_tv(mcp_handler_t *mcp);
/* 当前是否正在播放电视 */
int tv_is_playing(void);

#endif
