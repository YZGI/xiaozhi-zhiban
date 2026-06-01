import json
import os
import sys

_config = None

_DEFAULTS = {
    "device_ip": "192.168.1.1",
    "device_telnet_port": 23,
}

def _find_workspace_root():
    this_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(this_dir)

def get_config():
    global _config
    if _config is not None:
        return _config

    workspace_root = _find_workspace_root()
    config_path = os.path.join(workspace_root, "project_config.json")
    example_path = os.path.join(workspace_root, "project_config.example.json")

    if os.path.exists(config_path):
        with open(config_path, "r", encoding="utf-8") as f:
            _config = json.load(f)
    elif os.path.exists(example_path):
        print(f"提示: 未找到 {config_path}，使用示例配置。")
        print(f"  请复制 project_config.example.json 为 project_config.json 并修改设备IP等参数。")
        with open(example_path, "r", encoding="utf-8") as f:
            _config = json.load(f)
    else:
        print(f"警告: 未找到配置文件，使用内置默认值。")
        print(f"  请创建 {config_path}（可参考 project_config.example.json）。")
        _config = dict(_DEFAULTS)

    _config["_workspace_root"] = workspace_root

    return _config
