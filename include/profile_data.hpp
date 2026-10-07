// 编译期嵌入的默认配置与 /proc/cpuinfo 模板。
// 由 tools/embed_profile.py 生成 profile_data.cpp 提供定义。
#pragma once

#include <stddef.h>

namespace hw80 {

/// 极简的常量字节视图。
/// 刻意不用 std::string_view: Zygisk 模块最好完全不依赖 C++ 标准库
/// (官方模板用 APP_STL=none), 这样不会和宿主进程发生任何符号冲突。
struct StrRef {
    const char *ptr;
    size_t len;

    constexpr const char *data() const { return ptr; }
    constexpr size_t size() const { return len; }
};

extern const StrRef kDefaultConf;     // config/hw80pro.conf
extern const StrRef kDefaultCpuInfo;  // config/proc_cpuinfo.txt

} // namespace hw80
