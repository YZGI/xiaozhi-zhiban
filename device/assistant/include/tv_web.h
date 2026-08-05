#ifndef TV_WEB_H
#define TV_WEB_H

/**
 * 看电视本地网页按钮服务。
 *
 * 在设备内起一个轻量 HTTP 服务（默认端口 8082，可用环境变量
 * TV_WEB_PORT 覆盖）。浏览器或同局域网手机/电脑打开后，点页面上的
 * 「▶ 看电视 / ■ 关电视」按钮即可触发播放/停止，无需固件改动、无需
 * 云端，是最贴近「点桌面图标」的体验（固件只读，桌面图标无法新增）。
 *
 * 触发最终都走 mcp_play_tv / mcp_stop_tv（设备原厂 media_navi / splayer
 * 硬件解码链路），与离线命令词、触摸点击两条触发路径共用同一套实现。
 */

/* 启动网页按钮服务（创建 socket + 后台线程）。返回 0 成功，-1 失败。 */
int tv_web_start(void);

/* 停止网页按钮服务（关闭监听、回收线程）。 */
void tv_web_stop(void);

#endif /* TV_WEB_H */
