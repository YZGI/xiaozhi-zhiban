#!/bin/bash
# build_tv.sh —— 编译带 self.play_tv（看电视）功能的 sair
#
# ⚠️ 必须在 Linux / WSL 环境下运行！
#    Windows 本机无法编译：
#    1) 本机 WSL 已在系统层面损坏，无法使用；
#    2) QEMU 用户态模拟不支持 Windows 宿主（官方确认），qemu-user 跑不了 ARM 工具链；
#    3) 唯一办法是把源码 + 工具链放到一台正常的 WSL/Linux 机器上编译。
#
# 源码已内置 self.play_tv（device/assistant/src/mcp_handler.c，并在 tools/list 注册），
# 本脚本只负责定位工具链、调用 build.sh、并校验产物。
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ---- 自动定位 ARM uClibc 工具链 ----
if [ -n "$SDK_PATH" ]; then
  echo "▶ 使用 SDK_PATH=$SDK_PATH"
elif [ -d "$SCRIPT_DIR/../../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot" ]; then
  SDK_PATH="$SCRIPT_DIR/../../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot"
  echo "▶ 使用默认工具链: $SDK_PATH"
elif [ -d "$SCRIPT_DIR/../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot" ]; then
  SDK_PATH="$SCRIPT_DIR/../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot"
  echo "▶ 使用默认工具链: $SDK_PATH"
elif [ -f "$SCRIPT_DIR/../toolchain/tc.tar.xz" ]; then
  echo "▶ 发现 tc.tar.xz，自动解压 ARM uClibc 工具链..."
  mkdir -p "$SCRIPT_DIR/../toolchain"
  tar xf "$SCRIPT_DIR/../toolchain/tc.tar.xz" -C "$SCRIPT_DIR/../toolchain"
  if [ -d "$SCRIPT_DIR/../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot" ]; then
    SDK_PATH="$SCRIPT_DIR/../toolchain/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot"
  else
    SDK_PATH="$(find "$SCRIPT_DIR/../toolchain" -maxdepth 3 -name 'arm-buildroot-linux-uclibcgnueabi-gcc' 2>/dev/null | head -1 | xargs -r dirname 2>/dev/null)"
  fi
  [ -n "$SDK_PATH" ] && echo "▶ 使用解压出的工具链: $SDK_PATH"
else
  echo "❌ 找不到 ARM uClibc 工具链 (arm-buildroot-linux-uclibcgnueabi)" >&2
  echo "   请把 tc.tar.xz 放到: $SCRIPT_DIR/../toolchain/tc.tar.xz（脚本会自动解压）" >&2
  echo "   或 export SDK_PATH=/你的工具链/arm-buildroot-linux-uclibcgnueabi_sdk-buildroot" >&2
  exit 1
fi

# 校验关键二进制
if [ ! -x "$SDK_PATH/bin/arm-buildroot-linux-uclibcgnueabi-gcc" ]; then
  echo "❌ 工具链不完整：缺少 bin/arm-buildroot-linux-uclibcgnueabi-gcc" >&2
  exit 1
fi

export SDK_PATH
echo "=== 开始编译（含 self.play_tv 看电视功能）==="
bash "$SCRIPT_DIR/build.sh"

# ---- 校验产物 ----
SAIR="$SCRIPT_DIR/prebuilt/sair"
if [ ! -f "$SAIR" ]; then
  echo "❌ 编译产物缺失: $SAIR" >&2
  exit 1
fi

if command -v strings >/dev/null 2>&1 && strings "$SAIR" | grep -q "play_tv"; then
  echo "✅ 已确认二进制包含 self.play_tv 功能"
else
  echo "⚠️ 提示: 未在二进制中找到 'play_tv' 字符串（可能仍可用，请实测语音）"
fi

echo ""
echo "🎉 编译完成: $SAIR"
echo "   文件大小: $(stat -c%s "$SAIR" 2>/dev/null || echo '?') 字节"
echo ""
echo "下一步部署（设备需开机且已知 IP）:"
echo "   bash deploy_tv.sh <设备IP> $SAIR"
