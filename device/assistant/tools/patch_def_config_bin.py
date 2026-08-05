#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patch_def_config_bin.py — 给 GS705B 的 /etc/user/def_config.bin 追加"看电视"离线命令词

背景
----
GS705B 的语音唤醒引擎 libsair_asr.so 强制从 /etc/user/def_config.bin 读取唤醒词 /
本地命令词（源码里 asr_config_t.local_command_word[64] 只有 64 字节，放不下真实 15 词，
且引擎直接读二进制配置而非 app 传入的字段）。所以"加一个语音命令"必须改这个二进制文件，
不能只改源码。

本工具在 LOCAL_COMMAND_WORD 的值字符串（15 个拼音命令词，逗号分隔）末尾追加：
    ,kan dian shi,guan dian shi
即「看电视」与「关电视」两个新离线命令词。

安全策略
--------
1. 默认 **dry-run**（不写盘），只打印计划 + 周围字节，确认无误后加 --apply 才真正写。
2. 必须找到值字符串后面的 **连续 0x00 空闲区（slack）** 足够容纳新词 + 结尾 0x00，
   否则拒绝写入（防止踩到下一项字段）。空闲区不够时请改用 libapconfig.so 包装注入方案。
3. 写盘前自动备份为 <input>.bak.<时间戳>。
4. 幂等：若值里已有 kan dian shi 则跳过（已打过补丁）。

用法
----
  # 在 PC 上对本地的 def_config.bin 副本先做演练（推荐先这样看效果）
  python3 patch_def_config_bin.py def_config.bin

  # 确认输出无误后再真正写
  python3 patch_def_config_bin.py def_config.bin --apply

  # 直接对设备上的路径（需设备已挂载/可写，且本机有 python3）
  python3 patch_def_config_bin.py /etc/user/def_config.bin --apply

  # 顺带把文本备份也打同样补丁（config 库可能把 SHM 回写到 cfgbak）
  python3 patch_def_config_bin.py def_config.bin --apply --patch-text /etc/user/cfgbak.txt

注意
----
* 设备 BusyBox 没有 python，所以这台工具要在 **PC/Linux 上**对副本跑，
  再把改好的 def_config.bin 通过 telnet / scp 拷回设备对应路径。
* 打完补丁后**必须重启设备**（或至少重启 sair），让引擎重新读 config.bin 生效。
* config 库存在"运行时 SHM 回写"机制，可能用旧值覆盖刚改的 bin。若重启后命令词失效，
  请把 --patch-text 指向的 cfgbak.txt 也打好补丁，或直接在设备端改完立即重启且避免触发写回。
"""

import os
import sys
import time

# 设备真实 15 个本地命令词（拼音，与 def_config.bin 中 LOCAL_COMMAND_WORD 原值一致）
EXISTING_CMD_WORDS = (
    "sheng yin da yi dian,sheng yin xiao yi dian,shang yi shou,xia yi shou,"
    "ji xv bo fang,zan ting bo fang,xun huan bo fang,dan qv xun huan,sui ji bo fang,"
    "bang wo shou cang,qv xiao shou cang,sheng yin tiao dao zui da,sheng yin tiao dao zui xiao,"
    "bo fang ge qv,ting zhi bo fang"
)

# 要追加的新命令词（拼音）。kan dian shi=看电视(播放) guan dian shi=关电视(停止)
NEW_SUFFIX = b",kan dian shi,guan dian shi"

# 末尾锚点：最后一个原厂词（其后的 0x00 即值结束）。用于 15 词整体匹配失败时的兜底。
TAIL_ANCHOR = b"ting zhi bo fang"


def find_insert_point(data):
    """返回一个 (offset, slack_len) 或 None。offset 指向「值的 0x00 结尾」之后、
    即新内容应写入的起始位置；slack_len 是紧接着的连续 0x00 字节数。"""
    # 优先整串匹配（最精确）
    idx = data.find(EXISTING_CMD_WORDS.encode("ascii"))
    if idx >= 0:
        end = idx + len(EXISTING_CMD_WORDS)
        if data[end:end + 1] != b"\x00":
            # 整串后面不是 0x00，说明这不是配置值（可能是别的文本），放弃整串匹配
            idx = -1
        else:
            # 继续往后数连续 0x00
            j = end + 1
            while j < len(data) and data[j] == 0:
                j += 1
            return end + 1, j - (end + 1)

    # 兜底：仅用末尾词锚点
    idx = data.find(TAIL_ANCHOR)
    while idx >= 0:
        end = idx + len(TAIL_ANCHOR)
        if data[end:end + 1] == b"\x00":
            j = end + 1
            while j < len(data) and data[j] == 0:
                j += 1
            return end + 1, j - (end + 1)
        idx = data.find(TAIL_ANCHOR, idx + 1)

    return None


def patch_text_file(path):
    """对文本备份（cfgbak.txt）做等效补丁：找到值串，若没新词则追加。"""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    if "kan dian shi" in text:
        print(f"  [文本] {path} 已含 kan dian shi，跳过")
        return False
    if EXISTING_CMD_WORDS in text:
        text = text.replace(EXISTING_CMD_WORDS,
                            EXISTING_CMD_WORDS + ",kan dian shi,guan dian shi")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"  [文本] 已写入 {path}")
        return True
    print(f"  [文本] {path} 中未找到 LOCAL_COMMAND_WORD 值串，跳过")
    return False


def hexdump_around(data, off, before=24, after=48):
    start = max(0, off - before)
    end = min(len(data), off + after)
    chunk = data[start:end]
    line = ""
    for i, b in enumerate(chunk):
        if start + i == off:
            line += ">"
        line += "%02x " % b
    return line


def main():
    args = sys.argv[1:]
    apply = False
    patch_text_paths = []
    input_path = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--apply":
            apply = True
        elif a == "--patch-text":
            i += 1
            patch_text_paths.append(args[i])
        else:
            input_path = a
        i += 1

    if not input_path:
        print("用法: python3 patch_def_config_bin.py <def_config.bin> [--apply] [--patch-text <cfgbak.txt>]")
        sys.exit(2)

    if not os.path.isfile(input_path):
        print(f"❌ 文件不存在: {input_path}")
        sys.exit(1)

    with open(input_path, "rb") as f:
        data = f.read()

    print(f"读取 {input_path} ({len(data)} 字节)")

    if b"kan dian shi" in data:
        print("✅ 已包含 kan dian shi，def_config.bin 无需再打补丁（如需重打请先恢复备份）")
        if patch_text_paths:
            for p in patch_text_paths:
                if os.path.isfile(p):
                    patch_text_file(p)
        sys.exit(0)

    res = find_insert_point(data)
    if res is None:
        print("❌ 未在文件中定位到 LOCAL_COMMAND_WORD 值串（或其后非 0x00 结尾）。")
        print("   可能该设备的命令词列表与预设不同，请检查 strings 提取的真实值，")
        print("   或改用 libapconfig.so 包装注入方案（拦截 get_config 返回值）。")
        sys.exit(1)

    off, slack = res
    need = len(NEW_SUFFIX) + 1  # 新词 + 结尾 0x00
    print(f"定位到值结束位置 offset={off}，其后连续 0x00 空闲区={slack} 字节，需要 {need} 字节")
    print(f"周围字节: {hexdump_around(data, off)}")

    if slack < need:
        print(f"❌ 空闲区不足（{slack} < {need}），直接内联追加会踩到下一项字段，拒绝写入。")
        print("   请改用 libapconfig.so 包装注入：在 /var/upgrade 放同名 .so 优先加载，")
        print("   拦截 get_config(\"LOCAL_COMMAND_WORD\") 返回追加后的列表。")
        sys.exit(1)

    # 构造新内容：在 off 处写入 新词 + 0x00（覆盖原本的 0x00 及部分空闲区）
    new_data = data[:off] + NEW_SUFFIX + b"\x00" + data[off + need:]

    if not apply:
        print("【dry-run】未加 --apply，不写盘。计划写入:")
        print(f"   offset {off}: {NEW_SUFFIX.decode('ascii')} + 0x00")
        print(f"   新文件大小: {len(new_data)} 字节（原 {len(data)}）")
        print("确认无误后运行同一命令并加 --apply。")
        sys.exit(0)

    # 备份
    bak = f"{input_path}.bak.{int(time.time())}"
    with open(bak, "wb") as f:
        f.write(data)
    print(f"已备份原文件到 {bak}")

    with open(input_path, "wb") as f:
        f.write(new_data)
    print(f"✅ 已写入 {input_path}（{len(new_data)} 字节）")
    print("下一步：把该 def_config.bin 拷回设备 /etc/user/def_config.bin，然后【重启设备】生效。")

    for p in patch_text_paths:
        if os.path.isfile(p):
            patch_text_file(p)


if __name__ == "__main__":
    main()
