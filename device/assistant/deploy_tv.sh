#!/bin/bash
# deploy_tv.sh —— 把编译好的 sair 部署到 GS705B（经 xwebd :8080）
#
# 用法:
#   bash deploy_tv.sh <设备IP> <sair路径> [--cold]
#
# 默认走【热更新】(POST /api/assistant/upgrade)：秒级替换进程、不重启设备。
# 加 --cold 走【冷部署】(POST /api/assistant/deploy)：替换二进制 + 整机重启。
#
# 可在 Windows(Git Bash) 或 WSL/Linux 上运行，只要能访问设备 8080 端口即可。
set -e

if [ $# -lt 2 ]; then
  echo "用法: bash deploy_tv.sh <设备IP> <sair路径> [--cold]" >&2
  exit 1
fi

DEVICE_IP="$1"
SAIR="$2"
COLD=0
[ "$3" = "--cold" ] && COLD=1

PORT="${XIAOZHI_XWEBD_PORT:-8080}"
BASE="http://${DEVICE_IP}:${PORT}"

if [ ! -f "$SAIR" ]; then
  echo "❌ 找不到 sair: $SAIR" >&2
  exit 1
fi

echo "目标设备: $BASE"
echo "本地文件: $SAIR ($(stat -c%s "$SAIR" 2>/dev/null || echo '?') 字节)"
echo ""

echo "=== 1/2 上传 sair_new → $BASE/api/upload ==="
curl -sS -X POST "$BASE/api/upload" \
  -H "X-Filename: sair_new" \
  --data-binary @"$SAIR" \
  -w "\nHTTP %{http_code}\n"
echo ""

echo "=== 2/2 触发更新 ==="
if [ "$COLD" = "1" ]; then
  echo "冷部署（替换二进制 + 设备重启）: $BASE/api/assistant/deploy"
  curl -sS -X POST "$BASE/api/assistant/deploy" -w "\nHTTP %{http_code}\n"
else
  echo "热更新（秒级，进程替换，不重启）: $BASE/api/assistant/upgrade"
  curl -sS -X POST "$BASE/api/assistant/upgrade" -w "\nHTTP %{http_code}\n"
fi
echo ""

echo "等待 3 秒后查询状态..."
sleep 3
echo "=== 助手状态 ==="
curl -sS "$BASE/api/assistant/status" -w "\nHTTP %{http_code}\n" || echo "(状态查询失败，可稍后手动重试)"
echo ""
echo "🎬 部署完成。对设备说：『小智，看电视』试试看（默认播放 m3u8 测试流）。"
echo "   指定频道：『小智，播放 <某个 m3u8/rtsp 地址>』"
echo ""
echo "⚠️ 看门狗提示：若新 sair 连续 3 次启动即崩溃，设备会自动回退原厂固件。"
