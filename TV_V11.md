# 看电视 v11 —— 离线命令词方案（无需 App / 绕过云端）

> 适用：GS705B（智伴 1X）。让设备**本地语音**“看电视 / 关电视”直接出画面，不依赖小智 App、不依赖云端 AI 把话映射成工具调用。

---

## 为什么是 v11（与之前方案的区别）

- 之前 `self.play_tv` 只注册了**云端 MCP 工具**，靠云端 AI 把“看电视”映射成 `tools/call`。
  实测云端对这台自定义设备不可靠——日志里从无 `tools/call` 痕迹（续10 根因）。
- v11 改为：**设备本地离线命令词**触发（`libsair_asr` 引擎本地识别拼音命令词），直接调设备原厂媒体链路播放，**完全不经过云端对话**。
- 对齐用户的两条明确指向：
  1. 小智**没有 App** → 路径 A（App 文字触发）不可行，必须走设备本地。
  2. “和设备本身自带的 app 一样” → 用原厂播放机制：`media_navi_open` → `olmedia_service` 守护 → `libsmart_player_api` 硬解（学习软件同款栈）。

---

## 触发原理（一条链）

1. `libsair_asr` 引擎从 **`/etc/user/def_config.bin`** 读 `LOCAL_COMMAND_WORD`（原厂 15 个拼音命令词）。
2. 我们在末尾追加 `kan dian shi`(看电视) 与 `guan dian shi`(关电视) → 引擎本地识别为新的离线命令词。
3. 命中后回调 `on_wakeup_event(result = 命令词索引)` → `main.c` 的 `process_pending_wakeup` 按索引**直调** `mcp_play_tv` / `mcp_stop_tv`，**绕过 cloud 会话**。
4. `mcp_play_tv` 优先用 `libmedia_navi_api.so` 的 `media_navi_open`（设备原厂、学习软件同款）；若该库缺失/返回失败，自动回退 `libsmart_player_api.so` 的 `splayer_*`（已实证可从 sair 上下文调用）。

---

## 改动清单

| 文件 | 内容 |
|---|---|
| `include/xiaozhi_config.h` | `TV_DEFAULT_URL` / `TV_DEFAULT_VOL`、`TV_USE_MEDIA_NAVI`(默认 1)、`TV_PLAY_WAKEUP_INDEX`(16) / `TV_STOP_WAKEUP_INDEX`(17) |
| `include/mcp_handler.h` | `navi_handle` / `media_navi_open` / `media_navi_close` 字段；`mcp_play_tv` / `mcp_stop_tv` / `mcp_tv_is_playing` 声明 |
| `src/mcp_handler.c` | `init` 里 `dlopen libmedia_navi_api.so`；实现三个函数；`self.play_tv` 工具复用 `mcp_play_tv`（云端冗余触发） |
| `src/main.c` | `process_pending_wakeup` 顶部拦截电视命令（绕过云端）；TTS 播放时若电视在播则**抑制 AI 播报**避免与原声冲突 |
| `tools/patch_def_config_bin.py` | `def_config.bin` 安全补丁工具（dry-run + 空闲区检查 + 备份） |

---

## 一、给设备加“看电视”命令词（改 def_config.bin）

设备 BusyBox **没有 python**，工具在 **PC / Linux** 上跑：

```bash
# 1) 从设备拉取 def_config.bin（telnet 192.168.1.19 后用 cat/base64 拷出，或 scp）
# 2) PC 上先演练（不写盘，只打印计划 + 周围字节）
python3 tools/patch_def_config_bin.py def_config.bin
# 3) 确认输出无误后真正写入（写前自动备份为 .bak.<时间戳>）
python3 tools/patch_def_config_bin.py def_config.bin --apply
# 4) 把改好的文件拷回设备 /etc/user/def_config.bin
#    （telnet 里：/etc 通常可写；若只读先 mount -o remount,rw /）
# 5) 重启设备（让引擎重新读 config.bin 生效）
```

工具行为：
- 定位 `LOCAL_COMMAND_WORD` 值串 → 检查其后**连续 0x00 空闲区**是否 ≥ 28 字节；
- **不够则拒绝写入**并提示改用 `libapconfig.so` 包装注入（避免踩坏下一项字段）；
- 幂等：已含 `kan dian shi` 则跳过。

> ⚠️ **config 写回风险**：配置库有“运行时 SHM 回写”机制，可能用旧值覆盖刚改的 bin。若重启后命令词失效：
> - 把工具 `--patch-text` 指向 `/etc/user/cfgbak.txt` 一起打补丁；或
> - 改完**立即重启**，避免触发写回；或
> - 改用 `LD_LIBRARY_PATH` 优先加载包装版 `libapconfig.so`，在 `get_config("LOCAL_COMMAND_WORD")` 返回时追加（最稳，但实现更复杂）。

---

## 二、编译 sair（GitHub Actions 云编译，推荐）

```bash
git push origin develop      # 推送 device/** 改动 → Actions 自动构建
```

Actions 完成后在运行页 **Artifacts** 下载 `sair`。
（本地有 Linux/macOS 也可：`cd device/assistant && bash build_tv.sh`）

构建日志会打印 `✅ self.play_tv wired` 以及新的 `mcp_play_tv` / `media_navi_open` 校验。

---

## 三、部署

```bash
bash deploy_tv.sh <设备IP> /path/to/sair          # 热更新（秒级，不重启）
# 或
bash deploy_tv.sh <设备IP> /path/to/sair --cold   # 冷部署（替换二进制 + 整机重启）
```

---

## 四、语音验证 + 确认命令词索引

- 先说唤醒词再接「**看电视**」；或直接说「看电视」。应**直接出画面**，且不进入云端对话。
- 「**关电视**」停止播放。
- **确认命令词索引（关键）**：看设备日志里 `WAKEUP type=` 的值。
  - 若“看电视”对应的 `type` 不是 **16**，或“关电视”不是 **17**，改
    `include/xiaozhi_config.h` 的 `TV_PLAY_WAKEUP_INDEX` / `TV_STOP_WAKEUP_INDEX`
    为日志实际值，**重编重部署即可**（索引拦截逻辑无需动）。
- 改默认频道 / 音量：编辑 `xiaozhi_config.h` 的 `TV_DEFAULT_URL` / `TV_DEFAULT_VOL`，重走二~三步。

---

## 五、回退

- **看门狗**：新 `sair` 连续 3 次启动即崩溃，设备会自动回退原厂固件。本次改动不碰启动流程，正常不会触发。
- 用原厂 `sair` 再 `deploy_tv.sh` 一次，或面板“卸载助手”。
- 命令词：把 `def_config.bin` 的 `.bak` 备份还原后重启。

---

## 风险与备注

- `media_navi_open` 函数签名是按设备原厂惯例推断（`int media_navi_open(const char*)`），未拿到头文件。
  若实机播放异常（黑屏 / 崩溃），把 `TV_USE_MEDIA_NAVI` 改为 **0** 强制走 `splayer_*` 兜底，重编即可。
- 引擎命令词索引基数（1-based）是按“原厂 15 词 + 末尾追加”推断，**以日志实际值为准**（见第四步）。
- 离线命令词机制让“看电视 / 关电视”在**断网时也能用**——这正是无 App 场景最需要的能力。
