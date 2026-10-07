#!/system/bin/sh
# 卸载时清理模块数据目录。
# 如果你希望保留配置以便重装后继续使用, 把 rm -rf 那行注释掉。

DATA_DIR=/data/adb/hw80pro

rm -f /data/local/tmp/hw80pro_status.txt 2>/dev/null
rm -f /data/local/tmp/hw80pro.log 2>/dev/null

# rm -rf "$DATA_DIR" 2>/dev/null

exit 0
