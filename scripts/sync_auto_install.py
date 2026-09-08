#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync_auto_install.py — 注册/注销 开机自启 (完全静默后台同步守护)

用法:
  python scripts/sync_auto_install.py install [--watch 300]   # 注册开机自启
  python scripts/sync_auto_install.py uninstall               # 注销
  python scripts/sync_auto_install.py status                  # 查看状态

原理:
  写入 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run (登录自启, 免管理员):
    pythonw.exe scripts/sync_auto.py --silent --watch <N>
  pythonw = 无控制台窗口; --silent = 无输出, 日志写 D:\\MYP\\sync-auto.log;
  --watch N = 每 N 秒扫描一次, 有变化自动网页提交到 GitHub (与 git push 等效, 但零 git)。
"""
import argparse
import os
import sys
import winreg

RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
VALUE_NAME = "vtouch-sync-auto"


def pythonw_path():
    """与当前解释器同目录的 pythonw.exe (无窗口版)."""
    d = os.path.dirname(sys.executable)
    cand = os.path.join(d, "pythonw.exe")
    if os.path.exists(cand):
        return cand
    return os.path.join(d, "python.exe")  # 回退 (有控制台, 但 silent 下无输出)


def script_path():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "sync_auto.py"))


def install(watch):
    cmd = f'"{pythonw_path()}" "{script_path()}" --silent --watch {watch}'
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        winreg.SetValueEx(k, VALUE_NAME, 0, winreg.REG_SZ, cmd)
    print(f"[install] 已注册开机自启:\n  {cmd}")
    print("[install] 下次登录自动启动; 立即启动请运行:")
    print(f'  start "" {cmd}')


def uninstall():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
            winreg.DeleteValue(k, VALUE_NAME)
        print("[uninstall] 已注销开机自启")
    except FileNotFoundError:
        print("[uninstall] 未注册过")


def status():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_READ) as k:
            val, _ = winreg.QueryValueEx(k, VALUE_NAME)
        print(f"[status] 已注册: {val}")
    except FileNotFoundError:
        print("[status] 未注册")


def main():
    ap = argparse.ArgumentParser(description="vtouch 静默同步守护: 开机自启注册")
    ap.add_argument("action", choices=["install", "uninstall", "status"])
    ap.add_argument("--watch", type=int, default=300, help="同步间隔秒 (默认 300=5 分钟)")
    args = ap.parse_args()
    if args.action == "install":
        install(args.watch)
    elif args.action == "uninstall":
        uninstall()
    else:
        status()


if __name__ == "__main__":
    main()
