#ifndef XIAOZHI_ASSISTANT_CONFIG_H
#define XIAOZHI_ASSISTANT_CONFIG_H

#include "version.h"

#ifndef XIAOZHI_VERSION
#define XIAOZHI_VERSION "dev"
#endif
#define XIAOZHI_CHIP_MODEL "gs705b"

#define ACTIVATION_CHECK_INTERVAL_MS  5000
#define ACTIVATION_RETRY_INTERVAL_MS  30000
#define HTTP_RESPONSE_TIMEOUT_MS      20000
#define WAKEUP_COOLDOWN_MS            3000
#define POST_CLEANUP_COOLDOWN_MS      2000
#define TCP_CONNECT_TIMEOUT_MS        10000
#define TLS_HANDSHAKE_TIMEOUT_MS      15000
#define HELLO_TIMEOUT_MS              10000
#define LISTEN_TIMEOUT_MS             120000
#define SPEAK_TIMEOUT_MS              120000
#define SESSION_TIMEOUT_MS            300000
#define CLEANUP_TIMEOUT_MS            5000
#define WIFI_CHECK_INTERVAL_MS         5000
#define WATCHDOG_INTERVAL_MS          10000
#define MAIN_LOOP_TICK_MS             1000
#define WS_PING_INTERVAL_MS           25000
#define WS_IDLE_TIMEOUT_MS            60000

#define MSG_APP_QUIT             0x001
#define MSG_SAIR_ENABLE          0x23E
#define MSG_SAIR_DISABLE         0x23F
#define MSG_KEY_HOME             0x040
#define MSG_KEY_BACK             0x041

#define MSG_SAIR_OPEN            0x3E8
#define MSG_SAIR_CLOSE           0x3E9
#define MSG_SAIR_AI_START        0x3EB
#define MSG_SAIR_AI_STOP         0x3EC
#define MSG_SAIR_ASR_START       0x3ED
#define MSG_SAIR_ASR_STOP        0x3EE
#define MSG_SAIR_AEC_START       0x3EF
#define MSG_SAIR_AEC_STOP        0x3F0
#define MSG_SAIR_CHOOSE_AI       0x3F1
#define MSG_SAIR_REGISTER_CB     0x3F2
#define MSG_SAIR_STATUS_UPDATE   0x3F3
#define MSG_SAIR_GET_INFO        0x3F4
#define MSG_SAIR_POST_EVENT      0x3F5
#define MSG_SAIR_RECORD_REQUEST  0x3F6
#define MSG_SAIR_GET_EVENT       0x3F7

#define MSG_SAIR_AWAKE           0x235
#define MSG_SAIR_END             0x238
#define MSG_SAIR_AWAKE_CMD       0x239
#define MSG_SAIR_EMOTION         0x236

#define SUBTITLE_TYPE_NONE       0
#define SUBTITLE_TYPE_ASR        1
#define SUBTITLE_TYPE_TTS        2
#define SUBTITLE_MAX_LEN         512

#define MIC_SERVICE_SOCKET       "/tmp/service/mqtt_custom_server"
#define MIC_CMD_SET_ENABLE       0x2CF2
#define MIC_CMD_GET_STATUS       0x2CF3

#define DEFAULT_OTA_URL          "https://api.tenclass.net/xiaozhi/ota/"
#define DEFAULT_WS_URL           "wss://api.tenclass.net/xiaozhi/v1/"
#define PLOG_PATH                "/var/upgrade/xiaozhi.log"

#define LISTENING_MODE_AUTOSTOP  0
#define LISTENING_MODE_REALTIME  1

#define GOODIX_KEY_HOME    102
#define GOODIX_KEY_BACK     30
#define GOODIX_KEY_VOLUP   242
#define GOODIX_KEY_VOLDOWN 243

#define INJECT_KEY_VOLUP   115
#define INJECT_KEY_VOLDOWN 114

/* ===== 看电视（离线命令词，绕过云端对话） =====
 * 走设备原厂媒体链路（学习软件播放视频同款），不依赖小智 App / 云端 AI。
 * 触发：在 /etc/user/def_config.bin 的 LOCAL_COMMAND_WORD 末尾追加
 *       kan dian shi(看电视) 与 guan dian shi(关电视) 两个拼音命令词，
 *       引擎本地识别后 on_wakeup_event 收到对应索引，直调媒体播放。
 */
#ifndef TV_DEFAULT_URL
/* 默认电视源（公开 HLS 测试流，请改成你的频道 m3u8/rtsp 地址，国内源更稳） */
#define TV_DEFAULT_URL "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
#endif
#ifndef TV_DEFAULT_VOL
#define TV_DEFAULT_VOL 30
#endif
/* 优先使用设备原厂媒体导航库 libmedia_navi_api.so（media_navi_open，走 olmedia_service 守护 +
 * 缓冲 UI，学习软件同款）。若实机播放异常（如该库未随固件放出），改成 0 强制走
 * libsmart_player_api.so 的 splayer_* 兜底路径（已实证可从 sair 上下文调用）。 */
#ifndef TV_USE_MEDIA_NAVI
#define TV_USE_MEDIA_NAVI 1
#endif
/* 离线命令词在 LOCAL_COMMAND_WORD 中的索引（1-based，主唤醒词 zhi ban zhi ban 不计入）。
 * 原厂 15 个命令词为 1..15（ting zhi bo fang 收尾=15），我们在末尾追加
 *   kan dian shi(看电视)  -> 16
 *   guan dian shi(关电视) -> 17
 * 实机若不符：唤醒后看日志 "WAKEUP type=" 的值，据此修正下面两个常量即可（无需改索引逻辑）。 */
#ifndef TV_PLAY_WAKEUP_INDEX
#define TV_PLAY_WAKEUP_INDEX 16
#endif
#ifndef TV_STOP_WAKEUP_INDEX
#define TV_STOP_WAKEUP_INDEX 17
#endif

#define WIFI_STATE_CONNECTED 5
#define WIFI_INFO_SIZE       116

#define LOG_TAG "XIAOZHI"

#define LOG_E(tag, fmt, ...) fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) fprintf(stdout, "[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...) do {} while(0)

#endif
