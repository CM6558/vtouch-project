#!/system/bin/sh
# KernelSU post-fs-data hook: prepare private runtime directory only.
MODDIR=${0%/*}
RUNDIR=/data/adb/vtouch-merge
mkdir -p "$RUNDIR" 2>/dev/null || exit 0
chmod 0700 "$RUNDIR" 2>/dev/null || true
# Do not start the merger here. service.sh starts it after input devices exist.
