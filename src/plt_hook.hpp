// 自己实现的 PLT/GOT hook 引擎。
//
// 为什么不用 Zygisk 的 pltHookRegister:
//   Zygisk 的 AddrSpace 是在调用 pltHookCommit() 的那一刻对"已经映射的"库打补丁的,
//   而我们的代码跑在 postAppSpecialize(api 即将失效)或更晚, 那时应用自己的 .so
//   还没加载。所以这里自己实现:
//     - 解析 /proc/self/maps 找到所有已加载 ELF
//     - 解析每个 ELF 的 PT_DYNAMIC / DT_JMPREL, 把 .rela.plt 里的目标符号
//       对应的 GOT 槽直接改掉
//     - hook android_dlopen_ext / __loader_android_dlopen_ext / dlopen,
//       让后续加载的库也被补上
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

namespace hw80 {

/// 重置 hook 表(只在 preAppSpecialize 调用一次)。
void plt_hook_reset();

/// 注册一条"符号名 -> 替换实现"的规则。若 old_out 非空则写入原始实现指针
/// (/proc/self/maps 里已加载库中该符号的解析结果, 即"真正的 libc 实现")。
bool plt_hook_add(const char *symbol, void *replacement, void **old_out);

/// 对所有已加载库打补丁。返回被修改的 GOT 槽数量。
size_t plt_hook_apply();

/// 打补丁后加载进来的新库需要再补一次。返回是否有新库被处理。
size_t plt_hook_apply_new();

/// 是否成功安装过 dlopen 系列 hook(用于排错日志)。
bool plt_hook_dlopen_hooked();

} // namespace hw80
