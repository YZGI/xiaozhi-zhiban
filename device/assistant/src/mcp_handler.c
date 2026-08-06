/**
 * @file mcp_handler.c
 * @brief MCP（Model Context Protocol）处理器实现
 *
 * 实现MCP协议的设备端处理，包括：
 * - 动态加载设备控制库（libmsg_server_api.so）
 * - MCP工具调用处理（获取设备状态、设置音量/亮度、重启/关机等）
 * - IoT指令处理
 * - JSON-RPC协议响应
 * - 系统信息读取（/proc文件系统）
 *
 * 注意：MCP协议中的 "xiaozhi-assistant" 是服务名标识，保留不改
 */

#include "mcp_handler.h"
#include "plog.h"
#include "xiaozhi_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>

/* 看电视播放状态互斥锁：网页按钮线程(tv_web)与主循环(触摸/命令词)可能
   并发调用 mcp_play_tv/mcp_stop_tv/mcp_tv_is_playing，用递归锁保护。
   mcp_play_tv 内部会调用 mcp_stop_tv，故必须用 PTHREAD_MUTEX_RECURSIVE。 */
static pthread_mutex_t g_tv_mutex;

/* 默认电视源（公开 HLS 测试流，请改成你的频道 m3u8/rtsp 地址） */
#ifndef TV_DEFAULT_URL
#define TV_DEFAULT_URL "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8"
#endif
#ifndef TV_DEFAULT_VOL
#define TV_DEFAULT_VOL 30
#endif

/* 动态加载符号的宏，加载失败时输出警告日志 */
#define LOAD_SYM(h, name, type)                     \
    do                                              \
    {                                               \
        mcp->name = (type)dlsym(h, #name);          \
        if (!mcp->name)                             \
            PLOG_W("MCP", "符号未找到: %s", #name); \
    } while (0)

/* 看电视播放状态（供 main.c 抑制 AI 播报，避免与原声冲突） */
static int g_tv_playing = 0;

/* mp_* 播放器句柄（mp_open 返回，跨 play/stop 复用，stop 时关闭） */
static void *g_tv_handle = NULL;

/**
 * @brief 初始化MCP处理器
 * @param mcp MCP处理器实例指针
 * @return 0成功，-1失败
 *
 * 动态加载libmsg_server_api.so库并解析所需的函数符号
 */
int mcp_handler_init(mcp_handler_t *mcp)
{
    if (!mcp)
        return -1;
    memset(mcp, 0, sizeof(*mcp));

    /* 初始化看电视互斥锁（递归锁，mcp_play_tv 会内部调用 mcp_stop_tv） */
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_tv_mutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    /* 动态加载设备控制库 */
    mcp->lib_handle = dlopen("libmsg_server_api.so", RTLD_NOW);
    if (!mcp->lib_handle)
    {
        PLOG_E("MCP", "加载 libmsg_server_api.so 失败: %s", dlerror());
        return -1;
    }

    /* 加载音频控制函数 */
    LOAD_SYM(mcp->lib_handle, sound_set_sys_volume, int (*)(int));
    LOAD_SYM(mcp->lib_handle, sound_get_sys_volume, int (*)(void));
    LOAD_SYM(mcp->lib_handle, sound_set_sys_mute, int (*)(int));
    LOAD_SYM(mcp->lib_handle, sound_is_sys_mute, int (*)(void));

    LOAD_SYM(mcp->lib_handle, power_get_charge_status, int (*)(int *));
    LOAD_SYM(mcp->lib_handle, power_get_battery_cap, int (*)(void));
    LOAD_SYM(mcp->lib_handle, power_get_battery_voltage, int (*)(void));

    /* 加载TTS播放函数 */
    LOAD_SYM(mcp->lib_handle, sound_tts_play, void (*)(int));

    /* 加载视频播放库 (工厂硬件解码 libsmart_player_api.so) */
    mcp->player_handle = dlopen("libsmart_player_api.so", RTLD_NOW);
    if (mcp->player_handle)
    {
        LOAD_SYM(mcp->player_handle, splayer_open, int (*)(void));
        LOAD_SYM(mcp->player_handle, splayer_close, int (*)(void));
        LOAD_SYM(mcp->player_handle, splayer_stop, int (*)(void));
        LOAD_SYM(mcp->player_handle, splayer_set_file, int (*)(const char *));
        LOAD_SYM(mcp->player_handle, splayer_play, int (*)(void));
        LOAD_SYM(mcp->player_handle, splayer_set_volume, int (*)(int));
        PLOG_I("MCP", "已加载 libsmart_player_api.so");
    }
    else
    {
        PLOG_W("MCP", "加载 libsmart_player_api.so 失败: %s", dlerror());
    }

    /* 加载流媒体核心播放库 (libmusic_player_api.so)：smart_player / 学习软件
       真正播在线流的接口，支持 http/hls/rtsp 拉流。mp_open 自启播放器服务。 */
    mcp->music_handle = dlopen("libmusic_player_api.so", RTLD_NOW);
    if (mcp->music_handle)
    {
        LOAD_SYM(mcp->music_handle, mp_open, void *(*)(void));
        LOAD_SYM(mcp->music_handle, mp_close, int (*)(void *));
        LOAD_SYM(mcp->music_handle, mp_set_file, int (*)(void *, const char *));
        LOAD_SYM(mcp->music_handle, mp_play, int (*)(void *));
        LOAD_SYM(mcp->music_handle, mp_stop, int (*)(void *));
        LOAD_SYM(mcp->music_handle, mp_pause, int (*)(void *));
        LOAD_SYM(mcp->music_handle, mp_resume, int (*)(void *));
        LOAD_SYM(mcp->music_handle, mp_set_volume, int (*)(void *, int));
        PLOG_I("MCP", "已加载 libmusic_player_api.so (mp_open=%s)",
               mcp->mp_open ? "ok" : "缺失");
    }
    else
    {
        PLOG_W("MCP", "加载 libmusic_player_api.so 失败: %s (看电视将走 splayer 兜底)", dlerror());
    }

    /* 加载设备原厂媒体导航库 (libmedia_navi_api.so，学习软件播视频同款，
       走 olmedia_service 守护 + 缓冲 UI)。TV_USE_MEDIA_NAVI=1 时优先用于看电视。 */
    mcp->navi_handle = dlopen("libmedia_navi_api.so", RTLD_NOW);
    if (mcp->navi_handle)
    {
        LOAD_SYM(mcp->navi_handle, media_navi_open, int (*)(const char *));
        LOAD_SYM(mcp->navi_handle, media_navi_close, int (*)(void));
        PLOG_I("MCP", "已加载 libmedia_navi_api.so (media_navi_open=%s)",
               mcp->media_navi_open ? "ok" : "缺失");
    }
    else
    {
        PLOG_W("MCP", "加载 libmedia_navi_api.so 失败: %s (看电视将走 splayer 兜底)", dlerror());
    }

    PLOG_I("MCP", "初始化完成，已加载 libmsg_server_api.so");
    return 0;
}

/**
 * @brief 销毁MCP处理器，释放资源
 * @param mcp MCP处理器实例指针
 */
void mcp_handler_destroy(mcp_handler_t *mcp)
{
    if (!mcp)
        return;
    if (mcp->player_handle)
    {
        dlclose(mcp->player_handle);
        mcp->player_handle = NULL;
    }
    if (mcp->music_handle)
    {
        dlclose(mcp->music_handle);
        mcp->music_handle = NULL;
    }
    if (mcp->navi_handle)
    {
        dlclose(mcp->navi_handle);
        mcp->navi_handle = NULL;
    }
    if (mcp->lib_handle)
    {
        dlclose(mcp->lib_handle);
        mcp->lib_handle = NULL;
    }
    PLOG_I("MCP", "已销毁");
}

/* ===== 看电视：设备原厂媒体链路播放（离线命令词触发，绕过云端） ===== */

int mcp_tv_is_playing(void)
{
    int v;
    pthread_mutex_lock(&g_tv_mutex);
    v = g_tv_playing;
    pthread_mutex_unlock(&g_tv_mutex);
    return v;
}

int mcp_stop_tv(mcp_handler_t *mcp)
{
    int ret = -1;
    pthread_mutex_lock(&g_tv_mutex);
    /* 主路径：关闭 mp_* 播放器句柄（若上一轮用 mp_* 播的，仅出声） */
    if (g_tv_handle && mcp && mcp->mp_stop)
    {
        mcp->mp_stop(g_tv_handle);
        if (mcp->mp_close)
            mcp->mp_close(g_tv_handle);
        g_tv_handle = NULL;
        ret = 0;
        PLOG_I("TV", "停止播放 (mp_stop/mp_close)");
    }
    /* 视频链路：media_navi_open / splayer_* 都经 smart_player 服务渲染，
       用 splayer_stop 即可停止其画面/解码；media_navi_close 收掉导航上下文。 */
    if (mcp && mcp->splayer_stop)
    {
        mcp->splayer_stop();
        ret = 0;
        PLOG_I("TV", "停止播放 (splayer_stop)");
    }
    if (mcp && mcp->media_navi_close)
    {
        mcp->media_navi_close();
        ret = 0;
        PLOG_I("TV", "停止播放 (media_navi_close)");
    }
    g_tv_playing = 0;
    pthread_mutex_unlock(&g_tv_mutex);
    return ret;
}

int mcp_play_tv(mcp_handler_t *mcp, const char *url)
{
    if (!mcp)
        return -1;
    pthread_mutex_lock(&g_tv_mutex);
    if (!url || url[0] == '\0')
        url = TV_DEFAULT_URL;

    /* 先停掉上一路，避免画面/音频叠加（递归锁，内部再加锁安全） */
    mcp_stop_tv(mcp);

    int ok = -1;

    /* 主路径：原厂视频硬解链路 (libmedia_navi_api.so 的 media_navi_open)。
       这是设备自带"学习"应用播在线视频的同款接口：内部经 send_service_cmd 把
       播放指令发给常驻的 smart_player 服务，由它走 libve 硬解并渲染到屏幕（出画）。
       注意：mp_*（libmusic_player_api.so）只是音频库，只能出声、绝不出画，
       所以绝不能再当作主路径——那正是之前黑屏的根因。 */
    if (mcp->media_navi_open)
    {
        mcp->media_navi_open(url);
        PLOG_I("TV", "media_navi_open 播放(视频): %s", url);
        ok = 0;
    }
    else
    {
        PLOG_W("TV", "media_navi_open 不可用，回退 splayer_*");
    }

    if (ok != 0)
    {
        /* 兜底A：工厂硬解库 libsmart_player_api.so 的 splayer_*（把 URL 发给
           smart_player 服务，由它 media_navi_open 出画）。 */
        if (!mcp->splayer_set_file || !mcp->splayer_play)
        {
            PLOG_E("TV", "播放失败：media_navi_open 与 splayer 均不可用");
            g_tv_playing = 0;
            pthread_mutex_unlock(&g_tv_mutex);
            return -1;
        }
        if (mcp->splayer_open)
            mcp->splayer_open();
        mcp->splayer_set_file(url);
        if (mcp->splayer_set_volume)
            mcp->splayer_set_volume(TV_DEFAULT_VOL);
        mcp->splayer_play();
        PLOG_I("TV", "splayer_* 播放(视频兜底): %s", url);
        ok = 0;
    }

    if (ok != 0)
    {
        /* 最终兜底：流媒体核心 API (libmusic_player_api.so 的 mp_*)——仅出声
           （音频库），用于以上两条视频链路都不可用时的降级，至少能听到。 */
        if (mcp->mp_open && mcp->mp_set_file && mcp->mp_play)
        {
            void *h = mcp->mp_open();
            if (h)
            {
                g_tv_handle = h;
                mcp->mp_set_file(h, url);
                if (mcp->mp_set_volume)
                    mcp->mp_set_volume(h, TV_DEFAULT_VOL);
                mcp->mp_play(h);
                PLOG_I("TV", "mp_* 播放(音频兜底): %s", url);
                ok = 0;
            }
        }
    }

    g_tv_playing = (ok == 0) ? 1 : 0;
    pthread_mutex_unlock(&g_tv_mutex);
    return ok;
}

/**
 * @brief 设置JSON消息发送回调
 * @param mcp MCP处理器实例指针
 * @param send_cb 发送回调函数
 * @param user_data 传递给回调的用户数据
 */
void mcp_handler_set_send_cb(mcp_handler_t *mcp, mcp_send_json_cb_t send_cb, void *user_data)
{
    if (!mcp)
        return;
    mcp->send_json = send_cb;
    mcp->user_data = user_data;
}

/**
 * @brief 从JSON数据中查找指定键的字符串值
 * @param json JSON数据指针
 * @param len JSON数据长度
 * @param key 要查找的键名
 * @param out 输出缓冲区
 * @param out_size 输出缓冲区大小
 * @return 找到返回输出缓冲区指针，未找到返回NULL
 */
static const char *find_string(const char *json, size_t len, const char *key, char *out, int out_size)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *p = (const char *)memmem(json, len, search_key, strlen(search_key));
    if (!p)
        return NULL;

    p += strlen(search_key);
    while (p < json + len && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    if (p >= json + len || *p != '"')
        return NULL;
    p++;

    /* 提取字符串值，处理转义字符 */
    int i = 0;
    while (p < json + len && *p != '"' && i < out_size - 1)
    {
        if (*p == '\\' && p + 1 < json + len)
        {
            p++;
            switch (*p)
            {
            case 'n':
                out[i++] = '\n';
                break;
            case 'r':
                out[i++] = '\r';
                break;
            case 't':
                out[i++] = '\t';
                break;
            case '"':
                out[i++] = '"';
                break;
            case '\\':
                out[i++] = '\\';
                break;
            default:
                out[i++] = *p;
                break;
            }
        }
        else
        {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return out;
}

/**
 * @brief 从JSON数据中查找指定键的整数值
 * @param json JSON数据指针
 * @param len JSON数据长度
 * @param key 要查找的键名
 * @param default_val 默认值
 * @return 找到的整数值，未找到返回默认值
 */
static int find_int(const char *json, size_t len, const char *key, int default_val)
{
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char *p = (const char *)memmem(json, len, search_key, strlen(search_key));
    if (!p)
        return default_val;
    p += strlen(search_key);
    while (p < json + len && (*p == ' ' || *p == ':' || *p == '\t'))
        p++;
    return atoi(p);
}

/**
 * @brief 发送MCP成功响应
 * @param mcp MCP处理器实例指针
 * @param id 请求ID
 * @param text 响应文本内容
 */
static void send_mcp_response(mcp_handler_t *mcp, int64_t id, const char *text)
{
    if (!mcp->send_json)
        return;

    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}}",
                     (long long)id, text);

    mcp->send_json(json, n, mcp->user_data);
}

/**
 * @brief 发送MCP错误响应
 * @param mcp MCP处理器实例指针
 * @param id 请求ID
 * @param code 错误码
 * @param message 错误消息
 */
static void send_mcp_error(mcp_handler_t *mcp, int64_t id, int code, const char *message)
{
    if (!mcp->send_json)
        return;

    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%lld,\"error\":{\"code\":%d,\"message\":\"%s\"}}}",
                     (long long)id, code, message);

    mcp->send_json(json, n, mcp->user_data);
}

/**
 * @brief 从/proc文件中读取指定键的整数值
 * @param path /proc文件路径
 * @param key 要查找的键名
 * @return 找到的整数值，未找到返回-1
 */
static int read_proc_int(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    int val = -1;
    while (fgets(line, sizeof(line), f))
    {
        if (strstr(line, key) == line)
        {
            char *p = line + strlen(key);
            while (*p == ' ' || *p == ':')
                p++;
            val = atoi(p);
            break;
        }
    }
    fclose(f);
    return val;
}

static int inject_key_event(int key_code)
{
    int fd = open("/dev/input/event2", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
    {
        PLOG_W("MCP", "inject_key: 打开/dev/input/event2失败: %m");
        return -1;
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_KEY;
    ev.code = key_code;
    ev.value = 1;
    write(fd, &ev, sizeof(ev));
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_KEY;
    ev.code = key_code;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
    close(fd);
    return 0;
}

static int inject_key_repeat(int key_code, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (inject_key_event(key_code) < 0)
            return -1;
        if (i < count - 1)
            usleep(80000);
    }
    PLOG_I("MCP", "inject_key_repeat: code=%d count=%d", key_code, count);
    return 0;
}

static int exec_tool(mcp_handler_t *mcp, const char *name, const char *args_json, size_t args_len, char *result, int result_size)
{
    /* 获取设备状态：音量、亮度、充电状态、电池、CPU负载、内存 */
    if (strcmp(name, "self.get_device_status") == 0)
    {
        int vol = 0, charge_st = 0, battery = 0;
        if (mcp->sound_get_sys_volume)
            vol = mcp->sound_get_sys_volume();
        if (mcp->power_get_charge_status)
            mcp->power_get_charge_status(&charge_st);
        if (mcp->power_get_battery_cap)
            battery = mcp->power_get_battery_cap();

        int mem_total = read_proc_int("/proc/meminfo", "MemTotal");
        int mem_free = read_proc_int("/proc/meminfo", "MemFree");
        int cached = read_proc_int("/proc/meminfo", "Cached");

        char loadbuf[64] = "";
        FILE *lf = fopen("/proc/loadavg", "r");
        if (lf)
        {
            fgets(loadbuf, sizeof(loadbuf), lf);
            fclose(lf);
        }
        char *sp = strchr(loadbuf, ' ');
        if (sp)
            *sp = '\0';

        int mem_used_kb = (mem_total > 0 && mem_free >= 0) ? (mem_total - mem_free - (cached > 0 ? cached : 0)) : 0;
        int vol_pct = (vol >= 0 && vol <= 40) ? vol * 100 / 40 : 0;

        snprintf(result, result_size,
                 "volume=%d%% charging=%s battery=%d%% cpu_load=%s mem_used=%dKB mem_free=%dKB mem_total=%dKB",
                 vol_pct, charge_st ? "yes" : "no", battery,
                 loadbuf[0] ? loadbuf : "N/A",
                 mem_used_kb, mem_free >= 0 ? mem_free : 0, mem_total >= 0 ? mem_total : 0);
        return 0;
    }

    if (strcmp(name, "self.audio_speaker.volume_up") == 0)
    {
        int vol = 0;
        if (mcp->sound_get_sys_volume)
            vol = mcp->sound_get_sys_volume();
        int steps = (vol < 40) ? 2 : 0;
        int ret = -1;
        if (steps > 0)
            ret = inject_key_repeat(INJECT_KEY_VOLUP, steps);
        if (ret < 0 && mcp->sound_set_sys_volume && vol < 40)
        {
            mcp->sound_set_sys_volume(vol + 2);
            PLOG_I("MCP", "volume_up: 按键注入失败, 后备sound_set %d->%d", vol, vol + 2);
        }
        else
        {
            PLOG_I("MCP", "volume_up: 按键注入%d步 (vol=%d)", steps, vol);
        }
        snprintf(result, result_size, "volume up (%d->%d)", vol, vol < 40 ? vol + 2 : vol);
        return 0;
    }

    if (strcmp(name, "self.audio_speaker.volume_down") == 0)
    {
        int vol = 0;
        if (mcp->sound_get_sys_volume)
            vol = mcp->sound_get_sys_volume();
        int steps = (vol > 0) ? 2 : 0;
        int ret = -1;
        if (steps > 0)
            ret = inject_key_repeat(INJECT_KEY_VOLDOWN, steps);
        if (ret < 0 && mcp->sound_set_sys_volume && vol > 0)
        {
            mcp->sound_set_sys_volume(vol - 2);
            PLOG_I("MCP", "volume_down: 按键注入失败, 后备sound_set %d->%d", vol, vol - 2);
        }
        else
        {
            PLOG_I("MCP", "volume_down: 按键注入%d步 (vol=%d)", steps, vol);
        }
        snprintf(result, result_size, "volume down (%d->%d)", vol, vol > 0 ? vol - 2 : vol);
        return 0;
    }

    if (strcmp(name, "self.reboot") == 0)
    {
        snprintf(result, result_size, "rebooting");
        PLOG_I("MCP", "收到重启请求");
        if (mcp->sound_tts_play)
            mcp->sound_tts_play(10);
        system("reboot");
        return 0;
    }

    /* 关机 */
    if (strcmp(name, "self.poweroff") == 0)
    {
        snprintf(result, result_size, "powering off");
        PLOG_I("MCP", "收到关机请求");
        if (mcp->sound_tts_play)
            mcp->sound_tts_play(10);
        system("poweroff");
        return 0;
    }

    /* 获取系统信息 */
    if (strcmp(name, "self.get_system_info") == 0)
    {
        int vol = 0, charge_st = 0, battery = 0;
        if (mcp->sound_get_sys_volume)
            vol = mcp->sound_get_sys_volume();
        if (mcp->power_get_charge_status)
            mcp->power_get_charge_status(&charge_st);
        if (mcp->power_get_battery_cap)
            battery = mcp->power_get_battery_cap();

        snprintf(result, result_size,
                 "xiaozhi-assistant P5 | vol=%d/40 chg=%d bat=%d%%",
                 vol, charge_st, battery);
        return 0;
    }

    /* 清理垃圾文件和缓存 */
    if (strcmp(name, "self.clean_junk") == 0)
    {
        int freed_kb = 0;

        /* 清理/tmp目录下的日志和临时文件 */
        DIR *d = opendir("/tmp");
        if (d)
        {
            struct dirent *ent;
            char path[256];
            while ((ent = readdir(d)) != NULL)
            {
                if (ent->d_name[0] == '.')
                    continue;
                if (strstr(ent->d_name, "log.") || strstr(ent->d_name, ".log") ||
                    strstr(ent->d_name, ".tmp") || strstr(ent->d_name, ".bak") ||
                    strstr(ent->d_name, "core."))
                {
                    snprintf(path, sizeof(path), "/tmp/%s", ent->d_name);
                    struct stat st;
                    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                    {
                        freed_kb += (int)(st.st_size / 1024);
                        unlink(path);
                        PLOG_I("MCP", "已清理 %s (%lld 字节)", path, (long long)st.st_size);
                    }
                }
            }
            closedir(d);
        }

        /* 释放系统页面缓存 */
        int mem_free_before = read_proc_int("/proc/meminfo", "MemFree");

        system("echo 3 > /proc/sys/vm/drop_caches 2>/dev/null");

        int mem_free_after = read_proc_int("/proc/meminfo", "MemFree");
        int reclaimed = (mem_free_after >= 0 && mem_free_before >= 0) ? (mem_free_after - mem_free_before) : 0;

        snprintf(result, result_size,
                 "cleaned %dKB from tmp files, cache dropped, mem reclaimed ~%dKB",
                 freed_kb, reclaimed > 0 ? reclaimed : 0);
        return 0;
    }

    /* 列出所有可用的MCP工具 */
    if (strcmp(name, "self.get_mcp_tools") == 0)
    {
        snprintf(result, result_size,
                 "Available MCP tools: "
                 "1.self.get_device_status - Get device status (volume%%,battery,CPU,memory); "
                 "2.self.audio_speaker.volume_up - Increase volume by one step (shows native volume bar); "
                 "3.self.audio_speaker.volume_down - Decrease volume by one step (shows native volume bar); "
                 "4.self.get_system_info - Get system info; "
                 "5.self.clean_junk - Clean temp files and drop caches; "
                 "6.self.get_mcp_tools - List all MCP tools; "
                 "7.self.reboot - Reboot device (user only); "
                 "8.self.poweroff - Power off device (user only)");
        return 0;
    }

    /* 看电视：播放指定网络 URL（设备原厂媒体链路，绕过云端；
       云端 MCP 工具 self.play_tv 也走这里，作为离线命令词之外的冗余触发） */
    if (strcmp(name, "self.play_tv") == 0)
    {
        char url[1024] = {0};
        find_string(args_json, args_len, "url", url, sizeof(url));
        int r = mcp_play_tv(mcp, url[0] ? url : NULL);
        snprintf(result, result_size, r == 0 ? "playing %s" : "play failed",
                 url[0] ? url : TV_DEFAULT_URL);
        return r;
    }

    snprintf(result, result_size, "unknown tool: %s", name);
    return -1;
}

/**
 * @brief 处理MCP/IoT消息
 * @param mcp MCP处理器实例指针
 * @param json JSON消息字符串
 * @param len JSON消息长度
 *
 * 支持的消息类型：
 * - mcp: MCP协议消息（tools/call、tools/list、initialize等）
 * - iot: IoT设备控制消息（批量执行设备命令）
 */
void mcp_handler_process_message(mcp_handler_t *mcp, const char *json, size_t len)
{
    if (!mcp || !json || len == 0)
        return;

    char type_str[64] = {0};
    find_string(json, len, "type", type_str, sizeof(type_str));

    if (strcmp(type_str, "mcp") == 0)
    {
        /* 解析MCP消息的payload部分 */
        const char *payload_start = (const char *)memmem(json, len, "\"payload\"", 9);
        if (!payload_start)
        {
            PLOG_W("MCP", "MCP消息缺少payload");
            return;
        }
        const char *p = payload_start + 9;
        while (p < json + len && (*p == ' ' || *p == ':' || *p == '\t'))
            p++;
        if (p >= json + len || *p != '{')
            return;

        size_t payload_len = len - (p - json);
        char method[128] = {0};
        find_string(p, payload_len, "method", method, sizeof(method));

        int64_t id = find_int(p, payload_len, "id", -1);

        if (strcmp(method, "tools/call") == 0)
        {
            /* 处理工具调用请求 */
            const char *params_start = (const char *)memmem(p, payload_len, "\"params\"", 8);
            if (!params_start)
            {
                send_mcp_error(mcp, id, -32602, "missing params");
                return;
            }
            const char *pp = params_start + 8;
            while (pp < p + payload_len && (*pp == ' ' || *pp == ':' || *pp == '\t'))
                pp++;
            if (pp >= p + payload_len || *pp != '{')
                return;

            /* 解析工具名称和参数 */
            size_t params_len = payload_len - (pp - p);
            char tool_name[128] = {0};
            find_string(pp, params_len, "name", tool_name, sizeof(tool_name));

            /* 解析arguments对象 */
            const char *args_start = NULL;
            size_t args_len = 0;
            const char *a = (const char *)memmem(pp, params_len, "\"arguments\"", 11);
            if (a)
            {
                a += 11;
                while (a < pp + params_len && (*a == ' ' || *a == ':' || *a == '\t'))
                    a++;
                if (a < pp + params_len && *a == '{')
                {
                    args_start = a;
                    /* 通过括号深度匹配找到arguments对象的结束位置 */
                    const char *end = a;
                    int depth = 1;
                    while (end < pp + params_len && depth > 0)
                    {
                        if (*end == '{')
                            depth++;
                        else if (*end == '}')
                            depth--;
                        end++;
                    }
                    args_len = end - a;
                }
            }

            if (tool_name[0] == '\0')
            {
                send_mcp_error(mcp, id, -32602, "missing tool name");
                return;
            }

            /* 执行工具并返回结果 */
            char result[512];
            int ret = exec_tool(mcp, tool_name,
                                args_start ? args_start : json,
                                args_start ? args_len : len,
                                result, sizeof(result));

            PLOG_I("MCP", "工具=%s ID=%lld 结果=%s", tool_name, (long long)id, result);

            if (ret == 0)
            {
                send_mcp_response(mcp, id, result);
            }
            else
            {
                send_mcp_error(mcp, id, -32603, result);
            }
        }
        else if (strcmp(method, "tools/list") == 0)
        {
            /* 返回可用工具列表 */
            PLOG_I("MCP", "tools/list 请求, ID=%lld", (long long)id);
            if (mcp->send_json)
            {
                char json[3072];
                int n = snprintf(json, sizeof(json),
                                 "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%lld,"
                                 "\"result\":{\"tools\":["
                                 "{\"name\":\"self.get_device_status\",\"description\":\"Get the real-time status of the device including volume percentage, charging state, battery level, CPU load and memory usage\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.audio_speaker.volume_up\",\"description\":\"Increase the device volume by one step. This triggers the native volume button event so the volume bar animation is shown on screen. Use when user says turn up volume or make it louder\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.audio_speaker.volume_down\",\"description\":\"Decrease the device volume by one step. This triggers the native volume button event so the volume bar animation is shown on screen. Use when user says turn down volume or make it quieter\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.get_system_info\",\"description\":\"Get system information including version, volume and battery\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.clean_junk\",\"description\":\"Clean temporary files and drop system caches to free memory and improve performance\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.get_mcp_tools\",\"description\":\"List and describe all available MCP tools on this device\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                                 "{\"name\":\"self.reboot\",\"description\":\"Reboot the device\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}},\"annotations\":{\"audience\":[\"user\"]}},"
                                 "{\"name\":\"self.poweroff\",\"description\":\"Power off the device\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}},\"annotations\":{\"audience\":[\"user\"]}},"
                                 "{\"name\":\"self.play_tv\",\"description\":\"Play a TV/stream URL on the device screen with hardware decoder. Use when the user says watch TV, watch a channel, or play a streaming URL. Argument: url (string, the m3u8/hls/rtsp address; if omitted use the default channel).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Stream URL to play\"}}}}"
                                 "]}}}",
                                 (long long)id);
                mcp->send_json(json, n, mcp->user_data);
            }
        }
        else if (strcmp(method, "initialize") == 0)
        {
            /* MCP协议初始化响应 */
            PLOG_I("MCP", "初始化: 协议版本=2024-11-05");
            if (mcp->send_json)
            {
                char json[512];
                int n = snprintf(json, sizeof(json),
                                 "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%lld,"
                                 "\"result\":{\"protocolVersion\":\"2024-11-05\","
                                 "\"capabilities\":{\"tools\":{}},"
                                 "\"serverInfo\":{\"name\":\"xiaozhi-assistant\",\"version\":\"P5\"}}}}",
                                 (long long)id);
                mcp->send_json(json, n, mcp->user_data);
            }
        }
        else if (strcmp(method, "notifications/initialized") == 0)
        {
            /* 服务器初始化完成通知 */
            PLOG_I("MCP", "服务器初始化完成通知");
        }
        else
        {
            PLOG_W("MCP", "未知方法: %s", method);
        }
    }
    else if (strcmp(type_str, "iot") == 0)
    {
        /* 处理IoT设备控制消息 */
        PLOG_I("MCP", "IoT消息: %.*s", (int)(len > 200 ? 200 : len), json);

        /* 解析commands数组 */
        const char *cmd_start = (const char *)memmem(json, len, "\"commands\"", 10);
        if (!cmd_start)
            return;
        const char *c = cmd_start + 10;
        while (c < json + len && (*c == ' ' || *c == ':' || *c == '\t'))
            c++;
        if (c >= json + len || *c != '[')
            return;

        /* 找到数组结束位置 */
        const char *arr_end = c;
        int depth = 1;
        while (arr_end < json + len && depth > 0)
        {
            if (*arr_end == '[')
                depth++;
            else if (*arr_end == ']')
                depth--;
            arr_end++;
        }

        /* 逐个解析并执行命令 */
        const char *item = c + 1;
        while (item < arr_end - 1)
        {
            while (item < arr_end - 1 && (*item == ' ' || *item == ',' || *item == '\n' || *item == '\t'))
                item++;
            if (item >= arr_end - 1 || *item != '{')
                break;

            /* 通过括号深度匹配找到命令对象的结束位置 */
            const char *item_end = item;
            int item_depth = 1;
            while (item_end < arr_end - 1 && item_depth > 0)
            {
                if (*item_end == '{')
                    item_depth++;
                else if (*item_end == '}')
                    item_depth--;
                item_end++;
            }
            size_t item_len = item_end - item;

            /* 提取命令名称并执行 */
            char cmd_name[128] = {0};
            find_string(item, item_len, "name", cmd_name, sizeof(cmd_name));

            if (cmd_name[0])
            {
                char result[256];
                exec_tool(mcp, cmd_name, item, item_len, result, sizeof(result));
            }
            item = item_end;
        }
    }
}
