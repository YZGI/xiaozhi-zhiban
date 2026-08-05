/*
 * 看电视本地网页按钮 (v13 + 频道)
 *
 * 在设备内起一个轻量 HTTP 服务，浏览器或同局域网手机/电脑打开后点按钮
 * 即可触发看电视/关电视。固件只读、桌面图标无法新增，这是当前设备上
 * 最像「点图标」且完全可控的触发方式（sair 在可写 /var/upgrade，热部署）。
 *
 * 端口默认 8082，可用环境变量 TV_WEB_PORT 覆盖。
 * 所有触发最终都走 mcp_play_tv / mcp_stop_tv（设备原厂硬件解码链路），
 * 与离线命令词、触摸点击共用同一套实现，线程安全由 mcp_handler 的互斥锁保证。
 *
 * 频道 (v13.1):
 *  - /tv/play?url=XXX   播放指定频道地址（urldecode 后传给 mcp_play_tv）
 *  - /tv/channels       返回 /var/upgrade/tv_channels.json（频道列表，缺省返回 []）
 *  - /tv/status         额外返回当前 playing 的 url
 *  - 频道列表可由面板写入 /var/upgrade/tv_channels.json 同步给设备/手机浏览器
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/prctl.h>
#include "plog.h"
#include "mcp_handler.h"
#include "app_context.h"

#ifndef TV_WEB_PORT
#define TV_WEB_PORT 8082
#endif

/* 频道列表持久化路径（/var/upgrade 可写） */
#ifndef TV_CHANNELS_FILE
#define TV_CHANNELS_FILE "/var/upgrade/tv_channels.json"
#endif

static int g_tv_web_fd = -1;
static volatile int g_tv_web_running = 0;
static pthread_t g_tv_web_tid;

/* 当前播放的 url（用于 /tv/status 展示），仅本线程写入、读取加锁由调用方保证 */
static char g_tv_cur_url[512];

/* g_app 定义在 main.c；tv_web 线程复用其 mcp 上下文触发播放。 */
extern app_context_t g_app;

static const char *TV_WEB_HTML =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\">\n"
    "<title>看电视</title>\n"
    "<style>\n"
    "  *{box-sizing:border-box}\n"
    "  body{font-family:-apple-system,\"PingFang SC\",Helvetica,Arial,sans-serif;background:#111;color:#fff;margin:0;padding:24px;text-align:center}\n"
    "  h1{font-size:28px;margin:8px 0 18px}\n"
    "  select{width:90%;max-width:360px;padding:14px;font-size:18px;border-radius:12px;border:0;background:#222;color:#fff;margin-bottom:18px}\n"
    "  .row{display:flex;gap:16px;justify-content:center;flex-wrap:wrap}\n"
    "  button{flex:1;min-width:140px;max-width:260px;padding:30px 16px;font-size:26px;border:0;border-radius:18px;color:#fff;cursor:pointer}\n"
    "  .play{background:#e53935}\n"
    "  .stop{background:#455a64}\n"
    "  button:active{opacity:.8}\n"
    "  #status{margin-top:28px;font-size:18px;color:#9e9e9e}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>📺 看电视</h1>\n"
    "<select id=\"ch\"></select>\n"
    "<div class=\"row\">\n"
    "  <button class=\"play\" onclick=\"act('play')\">▶ 看电视</button>\n"
    "  <button class=\"stop\" onclick=\"act('stop')\">■ 关电视</button>\n"
    "</div>\n"
    "<div id=\"status\">状态：加载中…</div>\n"
    "<script>\n"
    "var CH={};\n"
    "function loadCh(){\n"
    "  fetch('/tv/channels').then(function(r){return r.json();}).then(function(list){\n"
    "    var sel=document.getElementById('ch');sel.innerHTML='';CH={};\n"
    "    list.forEach(function(c){var o=document.createElement('option');o.value=c.url;o.textContent=c.name;sel.appendChild(o);CH[c.url]=c.name;});\n"
    "    if(!list.length){var o=document.createElement('option');o.value='';o.textContent='默认频道';sel.appendChild(o);}\n"
    "  }).catch(function(){});\n"
    "}\n"
    "function act(op){\n"
    "  var url=document.getElementById('ch').value;\n"
    "  var u=(op==='play'&&url)?('/tv/play?url='+encodeURIComponent(url)):'/tv/'+op;\n"
    "  fetch(u).then(function(r){return r.json();}).then(function(d){update();})\n"
    "    .catch(function(e){document.getElementById('status').textContent='操作失败：'+e;});\n"
    "}\n"
    "function update(){\n"
    "  fetch('/tv/status').then(function(r){return r.json();}).then(function(d){\n"
    "    var txt='状态：'+(d.playing?'正在播放 ▶':'已关闭 ■');\n"
    "    if(d.playing&&d.url){txt+='  '+(CH[d.url]||d.url);}\n"
    "    document.getElementById('status').textContent=txt;\n"
    "  }).catch(function(){});\n"
    "}\n"
    "loadCh();setInterval(function(){loadCh();update();},5000);update();\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

/* 简单 urldecode（%XX 与 +），写入 dst（长度 dstlen），返回 dst */
static char *tv_web_urldecode(char *dst, const char *src, int dstlen)
{
    int i = 0, j = 0;
    while (src[i] && j < dstlen - 1)
    {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2]))
        {
            unsigned int v = 0;
            sscanf(src + i + 1, "%2x", &v);
            dst[j++] = (char)v;
            i += 3;
        }
        else if (src[i] == '+')
        {
            dst[j++] = ' ';
            i++;
        }
        else
        {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
    return dst;
}

/* 读取频道列表文件原始内容到 out（outlen）；不存在返回 "[]" */
static void tv_web_load_channels(char *out, int outlen)
{
    FILE *f = fopen(TV_CHANNELS_FILE, "rb");
    if (!f)
    {
        snprintf(out, outlen, "[]");
        return;
    }
    int n = (int)fread(out, 1, outlen - 1, f);
    if (n < 0)
        n = 0;
    out[n] = '\0';
    fclose(f);
    if (n == 0)
        snprintf(out, outlen, "[]");
}

static void tv_web_send(int fd, int code, const char *content_type, const char *body, int len)
{
    char header[320];
    const char *reason = (code == 200) ? "OK" : (code == 404 ? "Not Found" : "Error");
    int hl = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %d\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      code, reason, content_type, len);
    send(fd, header, hl, 0);
    if (body && len > 0)
        send(fd, body, len, 0);
}

static void tv_web_handle(int fd)
{
    char req[2048];
    int n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0)
        return;
    req[n] = '\0';

    char method[16];
    char path[256];
    char query[1500];
    query[0] = '\0';
    if (sscanf(req, "%15s %255s", method, path) < 2)
    {
        tv_web_send(fd, 400, "text/plain; charset=utf-8", "bad request", 11);
        return;
    }
    /* 分离 query 串 */
    char *q = strchr(path, '?');
    if (q)
    {
        *q = '\0';
        strncpy(query, q + 1, sizeof(query) - 1);
        query[sizeof(query) - 1] = '\0';
    }

    if (strcmp(path, "/tv/play") == 0)
    {
        char decoded[1500];
        const char *url = NULL;
        char *u = strstr(query, "url=");
        if (u)
        {
            tv_web_urldecode(decoded, u + 4, sizeof(decoded));
            if (decoded[0])
                url = decoded;
        }
        int ok = mcp_play_tv(&g_app.mcp, url);
        if (url)
            strncpy(g_tv_cur_url, url, sizeof(g_tv_cur_url) - 1);
        else
            g_tv_cur_url[0] = '\0';
        PLOG_I("TVWEB", "网页触发 看电视 (ok=%d)%s%s", ok, url ? " url=" : "", url ? url : "");
        char buf[1600];
        int bl;
        if (url)
            bl = snprintf(buf, sizeof(buf),
                          "{\"ok\":%d,\"playing\":%d,\"url\":\"%s\"}",
                          (ok == 0) ? 1 : 0, mcp_tv_is_playing(), url);
        else
            bl = snprintf(buf, sizeof(buf),
                          "{\"ok\":%d,\"playing\":%d,\"url\":null}",
                          (ok == 0) ? 1 : 0, mcp_tv_is_playing());
        tv_web_send(fd, 200, "application/json; charset=utf-8", buf, bl);
    }
    else if (strcmp(path, "/tv/stop") == 0)
    {
        mcp_stop_tv(&g_app.mcp);
        g_tv_cur_url[0] = '\0';
        PLOG_I("TVWEB", "网页触发 关电视");
        char buf[128];
        int bl = snprintf(buf, sizeof(buf), "{\"ok\":1,\"playing\":%d}", mcp_tv_is_playing());
        tv_web_send(fd, 200, "application/json; charset=utf-8", buf, bl);
    }
    else if (strcmp(path, "/tv/status") == 0)
    {
        char buf[640];
        const char *cur = g_tv_cur_url[0] ? g_tv_cur_url : "null";
        int bl;
        if (g_tv_cur_url[0])
            bl = snprintf(buf, sizeof(buf), "{\"playing\":%d,\"url\":\"%s\"}", mcp_tv_is_playing(), cur);
        else
            bl = snprintf(buf, sizeof(buf), "{\"playing\":%d,\"url\":null}", mcp_tv_is_playing());
        tv_web_send(fd, 200, "application/json; charset=utf-8", buf, bl);
    }
    else if (strcmp(path, "/tv/channels") == 0)
    {
        char body[4096];
        tv_web_load_channels(body, sizeof(body));
        tv_web_send(fd, 200, "application/json; charset=utf-8", body, (int)strlen(body));
    }
    else
    {
        tv_web_send(fd, 200, "text/html; charset=utf-8",
                    TV_WEB_HTML, (int)strlen(TV_WEB_HTML));
    }
}

static void *tv_web_run(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "tv_web");
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    while (g_tv_web_running)
    {
        int cfd = accept(g_tv_web_fd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0)
        {
            /* listen fd 被 tv_web_stop 关闭时 accept 会失败，循环随之退出 */
            if (g_tv_web_running)
                PLOG_W("TVWEB", "accept 失败: %s", strerror(errno));
            continue;
        }
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        tv_web_handle(cfd);
        close(cfd);
    }
    return NULL;
}

int tv_web_start(void)
{
    int port = TV_WEB_PORT;
    const char *env = getenv("TV_WEB_PORT");
    if (env && atoi(env) > 0)
        port = atoi(env);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        PLOG_E("TVWEB", "socket 创建失败: %s", strerror(errno));
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        PLOG_E("TVWEB", "bind :%d 失败: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0)
    {
        PLOG_E("TVWEB", "listen 失败: %s", strerror(errno));
        close(fd);
        return -1;
    }

    g_tv_web_fd = fd;
    g_tv_web_running = 1;
    g_tv_cur_url[0] = '\0';
    if (pthread_create(&g_tv_web_tid, NULL, tv_web_run, NULL) != 0)
    {
        PLOG_E("TVWEB", "pthread_create 失败: %s", strerror(errno));
        close(fd);
        g_tv_web_fd = -1;
        g_tv_web_running = 0;
        return -1;
    }
    PLOG_I("TVWEB", "看电视网页按钮已启动: http://0.0.0.0:%d/", port);
    return 0;
}

void tv_web_stop(void)
{
    g_tv_web_running = 0;
    if (g_tv_web_fd >= 0)
    {
        close(g_tv_web_fd);
        g_tv_web_fd = -1;
    }
    pthread_join(g_tv_web_tid, NULL);
    PLOG_I("TVWEB", "看电视网页按钮已停止");
}
