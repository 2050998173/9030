#!/system/bin/sh
# late_start service 阶段。伪装逻辑完全由 Zygisk 在目标进程里完成,
# 这里不做任何事, 只保留脚本以便将来扩展(例如把 logcat 里的 HW80Pro
# 日志转存到文件)。
#
# 如果你的 ROM 缺少 logcat 或你想保留一份日志, 取消下面几行的注释。

MODDIR=${0%/*}
DATA_DIR=/data/adb/hw80pro

# (
#     sleep 30
#     logcat -d -s HW80Pro > "$DATA_DIR/last_boot.log" 2>/dev/null
# ) &

exit 0
