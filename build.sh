#!/usr/bin/env bash
# 构建 HUAWEI Mate 80 Pro 伪装模块并打包成 Magisk / KernelSU / APatch 可刷入的 zip
#
# 用法:
#   ANDROID_NDK_HOME=/path/to/ndk ./build.sh
#   或
#   ./build.sh /path/to/ndk
#
# 产物: dist/HuaweiMate80ProSpoof-v<version>.zip
set -euo pipefail

cd "$(dirname "$0")"
ROOT="$PWD"
OUT="$ROOT/build"
DIST="$ROOT/dist"

MODULE_ID="$(grep '^id=' module.prop | cut -d= -f2)"
VERSION="$(grep '^version=' module.prop | cut -d= -f2 | tr -d ' ')"

# ---------- 定位 NDK ----------
NDK="${1:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [ -z "$NDK" ]; then
    for c in "$HOME/Android/Sdk/ndk"/* /usr/lib/android-sdk/ndk/* "$ANDROID_HOME/ndk"/*; do
        [ -d "$c" ] && NDK="$c"
    done
fi
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "错误: 找不到 Android NDK。请设置 ANDROID_NDK_HOME 或作为第一个参数传入路径。" >&2
    exit 1
fi
echo "==> NDK: $NDK"

HOST_TAG=""
case "$(uname -s)" in
    Linux*)  HOST_TAG=linux-x86_64 ;;
    Darwin*) HOST_TAG=darwin-x86_64 ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG=windows-x86_64 ;;
    *) echo "不支持的系统: $(uname -s)" >&2; exit 1 ;;
esac
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
CMAKE="$TOOLCHAIN/bin/cmake"
[ -x "$CMAKE" ] || CMAKE="$(command -v cmake || true)"
NINJA="$TOOLCHAIN/bin/ninja"
[ -x "$NINJA" ] || NINJA="$(command -v ninja || true)"

if [ -z "$CMAKE" ]; then echo "错误: 找不到 cmake" >&2; exit 1; fi
command -v zip >/dev/null 2>&1 || { echo "错误: 需要 zip 命令" >&2; exit 1; }

# ---------- 逐个 ABI 构建 ----------
rm -rf "$OUT" "$DIST"
mkdir -p "$DIST"

ABIS="${ABIS:-arm64-v8a armeabi-v7a}"

for ABI in $ABIS; do
    echo ""
    echo "============================================================"
    echo "==> 构建 $ABI"
    echo "============================================================"
    CONFIG_LOG="$OUT/config-$ABI.log"
    mkdir -p "$OUT"

    # 不要把输出丢进 /dev/null —— 配置失败时那样只会留下一个
    # 没有上下文的 "exit code 1"。全部打到 stdout, 同时存一份日志。
    if ! "$CMAKE" -S . -B "$OUT/$ABI" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM=android-28 \
        -DANDROID_NDK="$NDK" \
        -DCMAKE_BUILD_TYPE=Release \
        ${NINJA:+-G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA"} \
        2>&1 | tee "$CONFIG_LOG"
    then
        echo ""
        echo "!! cmake 配置失败 ($ABI)。最后 40 行日志:"
        tail -n 40 "$CONFIG_LOG"
        exit 1
    fi

    if ! "$CMAKE" --build "$OUT/$ABI" --parallel; then
        echo ""
        echo "!! 编译失败 ($ABI)。看上面的编译器报错。"
        exit 1
    fi

    if [ ! -f "$OUT/$ABI/libhw80pro.so" ]; then
        echo ""
        echo "!! $ABI 编译返回成功, 但没有产出 libhw80pro.so -- 这不应该发生。"
        exit 1
    fi
    ls -l "$OUT/$ABI/libhw80pro.so"
done

# ---------- 打包 ----------
STAGE="$OUT/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -r module.prop customize.sh post-fs-data.sh service.sh uninstall.sh action.sh webroot "$STAGE/" 2>/dev/null || true
mkdir -p "$STAGE/zygisk"

for ABI in $ABIS; do
    # Zygisk 约定: zygisk/<abi>.so (文件名就是 ABI 名, 不是 libhw80pro.so)
    cp "$OUT/$ABI/libhw80pro.so" "$STAGE/zygisk/$ABI.so"
done

# 默认配置文件(安装时由 customize.sh 拷贝到 /data/adb/<id>/)
mkdir -p "$STAGE/config"
cp config/hw80pro.conf "$STAGE/config/hw80pro.conf"
cp config/proc_cpuinfo.txt "$STAGE/config/proc_cpuinfo.txt"

ZIP="$DIST/HuaweiMate80ProSpoof-${VERSION}.zip"
rm -f "$ZIP"
( cd "$STAGE" && zip -q -r -X "$ZIP" . -x '.git*' )

echo
echo "==> 完成: $ZIP"
unzip -l "$ZIP"
