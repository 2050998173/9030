#!/system/bin/sh
# Magisk / KernelSU / APatch 安装脚本。
# 只做文件摆放, 不依赖 Android 运行时。

SKIPUNZIP=1
# SKIPUNZIP=1 时解压要自己做, 而安装环境不一定有独立的 unzip 可执行文件,
# 打开 ASH_STANDALONE 才能用 busybox 内置的 unzip。
ASH_STANDALONE=1
# 不要用 magisk 内置 busybox 的独立模式去改 $PATH, 保持默认即可。

ui_print "*********************************************"
ui_print "  HUAWEI Mate 80 Pro 全面伪装 (Zygisk)"
ui_print "  麒麟 9030 Pro / SGT-AL50"
ui_print "*********************************************"
ui_print ""

# MODPATH 是安装框架提供的目标目录(公共 API)。个别管理器只给 MODDIR, 兜底一下。
MODROOT="${MODPATH:-${MODDIR:-}}"
if [ -z "$MODROOT" ]; then
    MODROOT="${0%/*}"
fi

# ---------------------------------------------------------------------------
# 1. 把模块文件解出来
# ---------------------------------------------------------------------------
ui_print "- 释放模块文件"
unzip -o "$ZIPFILE" -x 'META-INF/*' -d "$MODROOT" >/dev/null 2>&1

if [ ! -f "$MODROOT/module.prop" ]; then
    ui_print "! 解压失败: 找不到 module.prop"
    abort "! 安装中止"
fi

# ---------------------------------------------------------------------------
# 2. 按设备架构挑选 Zygisk 库
#    ARCH 取值: arm | arm64 | x86 | x64
# ---------------------------------------------------------------------------
case "$ARCH" in
    arm64) ABI="arm64-v8a" ;;
    arm)   ABI="armeabi-v7a" ;;
    x64)   ABI="x86_64" ;;
    x86)   ABI="x86" ;;
    *)     ABI="" ;;
esac

if [ -z "$ABI" ]; then
    ui_print "! 不支持的架构: ARCH=$ARCH"
    abort "! 安装中止"
fi

if [ ! -f "$MODROOT/zygisk/$ABI.so" ]; then
    ui_print "! 安装包里缺少 zygisk/$ABI.so"
    ui_print "! (手工编译时请确认 ABIS 里包含了 $ABI)"
    abort "! 安装中止"
fi

# Zygisk 约定: 模块根目录下 zygisk/<abi>.so, 文件名就是 ABI 名。
# 只保留本机 ABI, 其余删掉以节省空间。
mkdir -p "$MODROOT/zygisk"
for f in "$MODROOT/zygisk"/*.so; do
    case "$f" in
        *"/$ABI.so") ;;
        *) [ -e "$f" ] && rm -f "$f" ;;
    esac
done
ui_print "- 已选择架构: $ABI"

# ---------------------------------------------------------------------------
# 3. 安装配置文件(已存在则不覆盖, 保留用户改动)
# ---------------------------------------------------------------------------
DATA_DIR=/data/adb/hw80pro
ui_print "- 安装配置到 $DATA_DIR"
mkdir -p "$DATA_DIR" 2>/dev/null
chmod 0700 "$DATA_DIR" 2>/dev/null

if [ -f "$MODROOT/config/hw80pro.conf" ]; then
    if [ -f "$DATA_DIR/hw80pro.conf" ]; then
        ui_print "  已存在 hw80pro.conf, 保留原有配置"
        ui_print "  新版默认配置已存为 hw80pro.conf.new"
        cp -f "$MODROOT/config/hw80pro.conf" "$DATA_DIR/hw80pro.conf.new"
    else
        cp -f "$MODROOT/config/hw80pro.conf" "$DATA_DIR/hw80pro.conf"
    fi
    chmod 0600 "$DATA_DIR/hw80pro.conf"
fi

if [ -f "$MODROOT/config/proc_cpuinfo.txt" ]; then
    if [ -f "$DATA_DIR/proc_cpuinfo.txt" ]; then
        cp -f "$MODROOT/config/proc_cpuinfo.txt" "$DATA_DIR/proc_cpuinfo.txt.new"
    else
        cp -f "$MODROOT/config/proc_cpuinfo.txt" "$DATA_DIR/proc_cpuinfo.txt"
    fi
    chmod 0600 "$DATA_DIR/proc_cpuinfo.txt"
fi

rm -rf "$MODROOT/config"

# ---------------------------------------------------------------------------
# 4. 权限
# ---------------------------------------------------------------------------
set_perm_recursive "$MODROOT" 0 0 0755 0644
set_perm "$MODROOT/zygisk/$ABI.so" 0 0 0644
[ -f "$MODROOT/service.sh" ] && set_perm "$MODROOT/service.sh" 0 0 0755
[ -f "$MODROOT/post-fs-data.sh" ] && set_perm "$MODROOT/post-fs-data.sh" 0 0 0755
[ -f "$MODROOT/action.sh" ] && set_perm "$MODROOT/action.sh" 0 0 0755
[ -f "$MODROOT/uninstall.sh" ] && set_perm "$MODROOT/uninstall.sh" 0 0 0755

ui_print ""
ui_print "- 安装完成, 请重启手机"
ui_print "- 配置文件: $DATA_DIR/hw80pro.conf"
ui_print "- 日志: logcat -s HW80Pro"
ui_print ""
