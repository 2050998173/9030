#!/system/bin/sh
# 开机早期执行(post-fs-data 阶段)。这里只需保证配置文件权限正确。
# 真正的伪装逻辑全部在 Zygisk 库里, 不需要在这里做任何事。

MODDIR=${0%/*}
DATA_DIR=/data/adb/hw80pro

if [ -d "$DATA_DIR" ]; then
    chmod 0700 "$DATA_DIR" 2>/dev/null
    chown 0:0 "$DATA_DIR" 2>/dev/null
    for f in "$DATA_DIR"/hw80pro.conf "$DATA_DIR"/proc_cpuinfo.txt; do
        [ -f "$f" ] && chmod 0600 "$f" 2>/dev/null && chown 0:0 "$f" 2>/dev/null
    done
fi

# 每次开机清掉上一次的日志
rm -f /data/local/tmp/hw80pro.log 2>/dev/null

exit 0
