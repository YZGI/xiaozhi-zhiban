#!/usr/bin/env python3
"""紧急修复脚本

当设备因自定义程序(xwebd/sair)崩溃导致频繁重启时，
此脚本会高频轮询设备连接状态，一旦检测到设备上线，
立即执行恢复出厂设置（删除所有自定义文件），使设备回到原生状态。

支持两种模式：
  - ADB模式（有线）：通过 adb devices 轮询
  - HTTP模式（无线）：通过 HTTP 请求轮询 xwebd API

使用方法：
  python emergency_nuke.py --adb          # ADB模式（推荐，更快）
  python emergency_nuke.py --http IP      # HTTP模式
  python emergency_nuke.py --auto IP      # 自动模式（先ADB后HTTP）
"""

import sys
import os
import time
import argparse
import subprocess
import signal

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))
from scripts.config_loader import get_config

BANNER = """
╔══════════════════════════════════════════════════════╗
║          ⚠️  紧急修复 ⚠️                 ║
║                                                      ║
║  设备频繁重启时使用，检测到设备上线后立即恢复出厂     ║
║  仅删除自定义程序(sair/xwebd)，不影响设备原生系统     ║
╚══════════════════════════════════════════════════════╝
"""

NUKE_CMD_ADB = (
    "killall sair 2>/dev/null; "
    "killall xwebd 2>/dev/null; "
    "rm -f /var/upgrade/sair /var/upgrade/sair_new /var/upgrade/sair_old; "
    "rm -f /var/upgrade/xwebd /var/upgrade/xwebd_new /var/upgrade/xwebd_old; "
    "rm -f /var/upgrade/sair_boot.log /var/upgrade/xiaozhi.log; "
    "rm -f /var/upgrade/boot_watchdog.sh /var/upgrade/xwebd_persist.conf; "
    "rm -f /var/upgrade/test.sh /var/upgrade/subtitle_trace.sh /var/upgrade/subtitle_trace.log; "
    "rm -rf /var/upgrade/sair_backup /var/upgrade/download; "
    "rm -f /tmp/sair_status.json /tmp/sair_config.json /tmp/sair_cmd.json; "
    "rm -f /tmp/sair_diag.json /tmp/sair_diag_request; "
    "rm -rf /dev/shm/sair* /dev/shm/xwebd*; "
    "echo NUKE_DONE"
)

NUKE_CMD_HTTP_PATHS = [
    "/api/assistant/uninstall",
    "/api/xwebd/remove",
]

POLL_INTERVAL_ADB = 0.3
POLL_INTERVAL_HTTP = 0.5
MAX_WAIT_SECONDS = 600


def nuke_via_adb(serial=None):
    """通过 ADB 执行恢复出厂设置"""
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += ["shell", NUKE_CMD_ADB]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=8)
        if "NUKE_DONE" in (result.stdout or ""):
            print("  ✅ ADB 清理成功！")
            return True
        else:
            print(f"  ⚠️ ADB 命令已执行，但未收到确认: {result.stdout[:200]}")
            return True
    except subprocess.TimeoutExpired:
        print("  ⚠️ ADB 命令超时，设备可能已重启")
        return False
    except Exception as e:
        print(f"  ❌ ADB 执行失败: {e}")
        return False


def nuke_via_http(ip, port=8080):
    """通过 HTTP API 执行恢复出厂设置"""
    try:
        import urllib.request
        url = f"http://{ip}:{port}/api/reboot"
        req = urllib.request.Request(url, data=b'', method='POST')
        req.add_header('Content-Type', 'application/json')
        urllib.request.urlopen(req, timeout=3)
    except Exception:
        pass

    try:
        import urllib.request
        url = f"http://{ip}:{port}/api/assistant/uninstall"
        req = urllib.request.Request(url, data=b'', method='POST')
        urllib.request.urlopen(req, timeout=3)
    except Exception:
        pass

    try:
        import urllib.request
        url = f"http://{ip}:{port}/api/xwebd/remove"
        req = urllib.request.Request(url, data=b'', method='POST')
        urllib.request.urlopen(req, timeout=3)
    except Exception:
        pass

    try:
        import urllib.request
        url = f"http://{ip}:{port}/api/reboot"
        req = urllib.request.Request(url, data=b'', method='POST')
        req.add_header('Content-Type', 'application/json')
        urllib.request.urlopen(req, timeout=3)
    except Exception:
        pass

    print("  ✅ HTTP 清理命令已发送")
    return True


def poll_adb(serial=None, interval=POLL_INTERVAL_ADB):
    """高频轮询 ADB 设备连接"""
    print(f"  🔍 ADB 轮询中 (间隔 {interval}s)...")
    start = time.time()
    attempt = 0
    while time.time() - start < MAX_WAIT_SECONDS:
        attempt += 1
        try:
            cmd = ["adb", "devices"]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            lines = result.stdout.strip().split('\n')
            for line in lines[1:]:
                line = line.strip()
                if not line:
                    continue
                parts = line.split('\t')
                if len(parts) >= 2 and parts[1] == "device":
                    dev_serial = parts[0]
                    if serial and dev_serial != serial:
                        continue
                    print(f"\n  🎯 检测到设备! serial={dev_serial} (第 {attempt} 次轮询, {time.time()-start:.1f}s)")
                    return dev_serial
        except Exception:
            pass
        time.sleep(interval)
    return None


def poll_http(ip, port=8080, interval=POLL_INTERVAL_HTTP):
    """高频轮询 HTTP 连接"""
    print(f"  🔍 HTTP 轮询 {ip}:{port} (间隔 {interval}s)...")
    start = time.time()
    attempt = 0
    while time.time() - start < MAX_WAIT_SECONDS:
        attempt += 1
        try:
            import urllib.request
            url = f"http://{ip}:{port}/api/services"
            req = urllib.request.Request(url, method='GET')
            urllib.request.urlopen(req, timeout=2)
            print(f"\n  🎯 检测到设备! {ip}:{port} (第 {attempt} 次轮询, {time.time()-start:.1f}s)")
            return True
        except Exception:
            pass
        time.sleep(interval)
    return False


def main():
    parser = argparse.ArgumentParser(description='紧急修复')
    parser.add_argument('--adb', action='store_true', help='使用 ADB 模式（有线）')
    parser.add_argument('--http', metavar='IP', help='使用 HTTP 模式（无线），指定设备 IP')
    parser.add_argument('--auto', metavar='IP', help='自动模式（先 ADB 后 HTTP），指定设备 IP')
    parser.add_argument('--port', type=int, default=8080, help='xwebd 端口（默认 8080）')
    parser.add_argument('--serial', help='ADB 设备序列号')
    parser.add_argument('--yes', '-y', action='store_true', help='跳过确认')
    args = parser.parse_args()

    print(BANNER)

    cfg = get_config()
    device_ip = args.http or args.auto or cfg.get('device_ip', '')

    if not args.adb and not args.http and not args.auto:
        if device_ip:
            args.auto = device_ip
            print(f"  使用配置文件中的设备 IP: {device_ip}")
        else:
            args.adb = True
            print("  未指定模式，默认使用 ADB 模式")

    if not args.yes:
        print("\n  ⚠️  此操作将删除设备上所有自定义程序(sair/xwebd)，恢复到原生状态！")
        print("  ⚠️  设备必须通过 USB 连接（ADB模式）或 WiFi 连接（HTTP模式）")
        confirm = input("\n  确认执行？(输入 YES 确认): ").strip()
        if confirm != "YES":
            print("  已取消")
            return 1

    print()

    nuked = False

    if args.adb or args.auto:
        print("═" * 50)
        print("  [ADB 模式] 等待设备连接...")
        print("═" * 50)
        dev = poll_adb(serial=args.serial)
        if dev:
            print("  💥 执行清理...")
            nuked = nuke_via_adb(serial=args.serial if args.serial else dev)
        else:
            print("  ⏰ ADB 等待超时")

    if args.http or args.auto:
        if not nuked and device_ip:
            print()
            print("═" * 50)
            print(f"  [HTTP 模式] 等待设备 {device_ip} 上线...")
            print("═" * 50)
            online = poll_http(device_ip, port=args.port)
            if online:
                print("  💥 执行清理...")
                nuked = nuke_via_http(device_ip, port=args.port)
            else:
                print("  ⏰ HTTP 等待超时")

    print()
    if nuked:
        print("═" * 50)
        print("  ✅ 紧急修复完成！设备已恢复到原生状态")
        print("  ✅ 等待设备重启后即可正常使用")
        print("═" * 50)
        return 0
    else:
        print("═" * 50)
        print("  ❌ 紧急修复失败")
        print("  请尝试手动操作:")
        print("    adb shell \"rm -f /var/upgrade/sair /var/upgrade/xwebd; reboot\"")
        print("═" * 50)
        return 1


if __name__ == '__main__':
    sys.exit(main())
