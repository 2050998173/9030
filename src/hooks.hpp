// 各个 hook 模块的安装入口。所有安装函数都在 preAppSpecialize / postAppSpecialize
// 里被调用, 调用顺序有意义, 见 zygisk_module.cpp。
#pragma once

#include <jni.h>

// 只需要用到 zygisk::Api 的公开成员函数 hookJniNativeMethods,
// 因此前置声明即可, 不必把 zygisk.hpp 扩散到每个文件。
namespace zygisk {
struct Api;
}

namespace hw80 {

/// 解析所有原始函数指针 (必须在打补丁之前完成)。
void hook_resolve_originals();

/// 解析文件覆盖用到的原始函数指针(open/read/fopen...)。
/// 由 hook_resolve_originals() 调用。
void file_overlay_resolve_originals();

/// 系统属性读取拦截: __system_property_get / _read / _read_callback
void hook_install_property();

/// __system_property_find 句柄伪造 (实验性, 默认关闭)。
void hook_install_property_find();

/// 虚拟文件内容替换: open / openat / fopen / read / pread64 / fread / fgets /
/// fseek / fclose。
void hook_install_file_overlay();

/// 其它: uname 伪装 / getprop 子进程命令行改写。
void hook_install_misc();

/// Java 层 android.os.SystemProperties 的 native 方法替换
/// (只能在 postAppSpecialize 里调用, 那时类加载器已就绪)。
void hook_install_jni(JNIEnv *env, zygisk::Api *api);

} // namespace hw80
