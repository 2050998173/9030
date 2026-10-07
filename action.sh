#!/system/bin/sh
# 模块"操作"按钮(以及 WebUI 状态页)的入口。
# 生成一份便于核对的状态报告, 写到 /data/local/tmp/hw80pro_status.txt,
# 然后打印到终端。

MODDIR=${0%/*}
DATA_DIR=/data/adb/hw80pro
CONF="$DATA_DIR/hw80pro.conf"
STATUS=/data/local/tmp/hw80pro_status.txt

{
    echo "HUAWEI Mate 80 Pro 伪装模块 — 状态"
    echo "======================================"

    if [ -f "$MODDIR/module.prop" ]; then
        grep -E '^(version|versionCode)=' "$MODDIR/module.prop" 2>/dev/null
    fi

    echo ""
    echo "[Zygisk 库]"
    ZYGISK_LIB=""
    for f in "$MODDIR/zygisk"/*.so; do
        [ -f "$f" ] && ZYGISK_LIB="$f"
    done
    if [ -n "$ZYGISK_LIB" ]; then
        echo "  已安装: $ZYGISK_LIB"
    else
        echo "  缺失! 请重新安装模块"
    fi

    echo ""
    echo "[配置文件] $CONF"
    if [ -f "$CONF" ]; then
        echo "  大小: $(wc -c < "$CONF" 2>/dev/null) 字节"
        echo "  属性条数: $(grep -cE '^[A-Za-z0-9._-]+\..*=' "$CONF" 2>/dev/null)"
        echo ""
        echo "  伪装范围:"
        grep -E '^apps=' "$CONF" 2>/dev/null | sed 's/^/    /'
        echo ""
        echo "  cpuinfo 模式:"
        grep -E '^cpuinfo=' "$CONF" 2>/dev/null | sed 's/^/    /'
    else
        echo "  不存在 — 模块将使用编译内置的默认配置"
    fi

    echo ""
    echo "[内核 /proc/cpuinfo 片段(真机)]"
    grep -m1 -E '^Hardware' /proc/cpuinfo 2>/dev/null | sed 's/^/  /'
    echo "  逻辑 CPU 数: $(grep -c ^processor /proc/cpuinfo 2>/dev/null)"

    echo ""
    echo "[本机真实机型]"
    echo "  ro.product.model = $(getprop ro.product.model 2>/dev/null)"
    echo "  ro.product.brand = $(getprop ro.product.brand 2>/dev/null)"

    echo ""
    echo "[模块运行日志] (最近 40 行, tag=HW80Pro)"
    echo "  用下面命令实时查看:"
    echo "    adb shell logcat -s HW80Pro"
    logcat -d -s HW80Pro 2>/dev/null | tail -n 40 | sed 's/^/  /'
    echo ""
    echo "提示: 伪装只对配置里 apps= 列出的应用生效,"
    echo "      用 getprop 在终端里查是看不到伪装的(那是另一个进程)。"
} > "$STATUS" 2>&1

chmod 0644 "$STATUS" 2>/dev/null
cat "$STATUS"

exit 0
