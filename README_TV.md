# 小智智伴「看电视」(self.play_tv) 编译与部署指南

> 目标：让 GS705B（ARM32 Cortex-A5 soft-float / uClibc）上的 xiaozhi-zhiban 语音助手支持
> 语音「看电视」——小智语音驱动工厂硬件解码 `libsmart_player_api.so` 播放指定网络流。

> 📌 **推荐用 v11 离线命令词方案**：无需小智 App、不依赖云端 AI 把话映射成工具调用。
> 见 **`TV_V11.md`**（本地拼音命令词「看电视 / 关电视」直触播放，设备原厂 `media_navi_open` 链路）。
> 本文件保留的是早期“云端 MCP 工具”思路，仅供对照。

---

## ⚠️ 重要前提：为什么必须在本机「以外」的 Linux 环境编译

本机（Windows LTSC 19044）**无法在本机完成交叉编译**，原因已逐一排查确认：

1. **WSL 在本机系统层面损坏**：`wsl` 所有命令（含 `--help`）均返回 “系统找不到指定的文件”(ERROR_FILE_NOT_FOUND)。
   已排查服务、VC++ 运行库、二进制完整性、PATH、ASCII 路径、编码，均正常，确认 OS 层损坏，LTSC 无商店/更新源无法离线修复。
2. **QEMU 用户态模拟不支持 Windows 宿主**：官方确认 “QEMU user mode emulator is not yet available for Windows host,
   there is no plan to add such a feature”，因为 Linux syscall ABI 在 Windows 上不稳定/无文档。
   Windows 版 qemu 只能做**整机虚拟化**（系统模拟），而本机带宽为 ~60KB/s，下载 Ubuntu 镜像不现实。
3. 结论：**编译不能在本机 Windows 上做**，但你有两条「本机以外」的路：
   - ✅ **推荐：GitHub Actions 云编译**（见下方「方式 C」）—— 仓库已配好工作流，push 后 GitHub 的 Linux 机器自动编出 `sair`，本机零编译、零安装。
   - 备选：你另一台正常的 WSL / Linux 机器（见「方式 A / B」）。
   - 部署则可以从任意能访问设备 `8080` 端口的机器执行（Windows Git Bash 或 WSL 均可）。

> 源码已内置 `self.play_tv`，位于 `device/assistant/src/mcp_handler.c`（实现于第 521 行，
> 并在 `tools/list` 第 677 行注册）。**无需改任何代码**，正常构建即可包含该功能。

---

## 步骤一：准备编译环境（二选一）

### 方式 A：用 `tv_build_pack.zip`（最省事，推荐）

1. 把 `tv_build_pack.zip` 复制到你的 WSL/Linux 机器并解压：
   ```bash
   unzip tv_build_pack.zip -d tv_build && cd tv_build
   ```
2. 解压后的目录结构为 `tv_build_pack/assistant/`（源码+脚本）与
   `tv_build_pack/toolchain/tc.tar.xz`（工具链压缩包）。
3. **直接运行 `build_tv.sh` 即可** —— 脚本发现 `../toolchain/tc.tar.xz` 会自动解压工具链，
   无需手动操作。

### 方式 B：用完整源码树

1. 把整个 `xiaozhi-zhiban-dev` 目录复制到你的 WSL/Linux 机器。
   （脚本 `build_tv.sh` / `deploy_tv.sh` 已随本目录放在 `device/assistant/` 下。）
2. 准备 ARM uClibc 工具链：把 `tc.tar.xz` 解压到
   `xiaozhi-zhiban-dev/device/toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot`
   （解压后应有 `bin/arm-buildroot-linux-uclibcgnueabi-gcc` 和
   `arm-buildroot-linux-uclibcgnueabi/sysroot`）。
3. 或改用环境变量：`export SDK_PATH=/你的工具链/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot`

### 方式 C：GitHub Actions 云编译（推荐，本机零编译）

仓库已内置 `.github/workflows/build_sair.yml`。push 后在 GitHub 的 **Linux runner 上原生运行 buildroot 工具链**编译出 `sair`，
并把产物作为 **Artifact** 下发。本机（Windows）无需安装任何编译器/虚拟机。

> 前置已就绪：本仓库已包含 `device/toolchain/tc.tar.xz`（56MB ARM uClibc 工具链，已加入 `.gitignore` 白名单），
> 且 `remote` 已指向你的 GitHub 仓库（`origin`）。

1. **提交并推送**（本机已在 `develop` 分支完成所有改动提交，直接推）：
   ```bash
   git push origin develop
   ```
   > 若远端 `develop` 比本地新导致拒绝，先 `git pull --rebase origin develop` 再推。
2. 推送后 **Actions 自动触发**（push 触及 `device/**`）；也可手动：仓库 → **Actions** → `Build sair` → **Run workflow**。
3. 构建完成后：该次运行页 → **Artifacts** → 下载 `sair`（即编译好的二进制）。
4. 下载后按「步骤四」部署：`bash deploy_tv.sh <设备IP> ~/Downloads/sair`

编译日志会打印 `✅ self.play_tv wired` 与 `Strip: ... -> ... bytes`，确认看电视功能已编入。

> 说明：runner 上工具链为**静态链接**，零主机依赖；`tc.tar.xz` 由工作流自动解压到 `device/toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot`，
> 然后 `bash build.sh` 编译 20 个 .c 并链接。

## 步骤三：编译

```bash
cd xiaozhi-zhiban-dev/device/assistant
bash build_tv.sh
```

脚本会：自动定位工具链 → 调用 `build.sh` 编译 20 个 .c 并链接 → 校验产物含 `play_tv`。
产物：`device/assistant/prebuilt/sair`

## 步骤四：部署到 GS705B（xwebd :8080 两步）

设备需开机且你知道它的 IP（面板或路由器查看）。

```bash
# 从能访问设备 8080 的机器执行（Windows Git Bash 或 WSL 均可）
bash deploy_tv.sh <设备IP> /path/to/prebuilt/sair
```

脚本内部两步：
1. `POST /api/upload`（头 `X-Filename: sair_new`）→ 写 `/var/upgrade/sair_new`
2. `POST /api/assistant/upgrade` → **热更新**（秒级、进程替换、不重启）；加 `--cold` 则走
   `POST /api/assistant/deploy` → **冷部署**（替换二进制 + 整机重启）

## 步骤五：语音验证

- 对设备说「小智，看电视」→ 播放默认 m3u8 测试流（`TV_DEFAULT_URL`）。
- 「小智，播放 <某个 m3u8/rtsp 地址>」→ 播放指定频道。
- 默认音量 30，遥控器/面板可调节。

---

## ⚠️ 安全提示（看门狗）

- 若新 `sair` **连续 3 次启动即崩溃**，设备会**自动删除 `/var/upgrade/sair` 并回退原厂固件**。
- 本次改动仅在运行时注册了一个 MCP 工具、不影响启动流程，正常不会触发。
- 如需回退：用原厂 `sair` 再跑一次 `deploy_tv.sh`，或面板里「卸载助手」。

## 改默认频道 / 音量

编辑 `device/assistant/src/mcp_handler.c` 顶部（约第 29–33 行）：

```c
#define TV_DEFAULT_URL "https://你的.m3u8"
#define TV_DEFAULT_VOL 30
```

保存后重新走 步骤三 ~ 四。

---

## 附：本机已做过的工作（备查）

- 已解包 ARM uClibc 工具链到 `E:\tc_root\arm-buildroot-linux-uclibcgnueabi_sdk-buildroot`（全静态 ELF）。
- 已确认 `build.sh` 编译全部 20 个 .c（含 `mcp_handler.c`），链接 `-Wl,--unresolved-symbols=ignore-all`，
  并将产物拷到 `prebuilt/sair`。
- 本机尝试用 qemu-user 跑工具链失败（QEMU 用户态不支持 Windows），故转交 WSL 机器编译。
