// 基础工具: 日志、无依赖的字符串/内存操作、符号解析、/proc/self/maps 解析。
//
// 重要约定: 本文件里所有"内部"文件访问都走 raw syscall(sys_*), 绝对不经过
// libc 的 open/read 包装函数。因为模块会 PLT hook open/read, 如果内部逻辑再调用
// 被 hook 的版本就会无限递归。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>

namespace hw80 {

// ---------------------------------------------------------------- 日志 -----
// 通过 dlsym 拿 __android_log_print, 拿不到就退化成什么都不做。
void log_init();
void log_printf(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGI(...) ::hw80::log_printf(4 /*ANDROID_LOG_INFO*/, __VA_ARGS__)
#define LOGW(...) ::hw80::log_printf(5 /*ANDROID_LOG_WARN*/, __VA_ARGS__)
#define LOGE(...) ::hw80::log_printf(6 /*ANDROID_LOG_ERROR*/, __VA_ARGS__)

void log_set_debug(bool on);
bool log_debug_enabled();
#define LOGD(...)                                                     \
    do {                                                              \
        if (::hw80::log_debug_enabled())                              \
            ::hw80::log_printf(3 /*ANDROID_LOG_DEBUG*/, __VA_ARGS__); \
    } while (0)

// ------------------------------------------------- 内部文件 IO (raw syscall) --
/// 直接走 syscall, 不经过 libc 的 open/openat/read/cloexec 包装, 因此不受 hook 影响。
int sys_open_read(const char *path);
ssize_t sys_read(int fd, void *buf, size_t count);
int sys_close(int fd);
/// 读整个文件到 buf(自动补 '\0'), 返回实际字节数, 失败返回 -1。
long sys_read_file(const char *path, char *buf, size_t cap);

// ------------------------------------------------------- 字符串 / 内存 -----
size_t str_len(const char *s);
bool str_eq(const char *a, const char *b);
bool str_starts_with(const char *s, const char *prefix);
bool str_ends_with(const char *s, const char *suffix);
bool str_contains(const char *haystack, const char *needle);
void *mem_copy(void *dst, const void *src, size_t n);
void *mem_set(void *dst, int c, size_t n);

/// 把 src 复制进 dst(最多 cap 字节, 含结尾 '\0'), 返回写入的长度。
size_t copy_to_buf(char *dst, size_t cap, const char *src);
size_t copy_to_buf(char *dst, size_t cap, const char *src, size_t src_len);

long parse_long(const char *s, long fallback);
unsigned long parse_hex(const char *s, unsigned long fallback);
bool parse_bool(const char *s);
/// 去掉首尾空白(原地修改), 返回首指针。
char *trim(char *s);

// ---------------------------------------------------------------- 符号 -----
/// 在已加载的模块里查符号。跳过本模块自身导出的同名符号,
/// 这样 hook 里拿到的才是真正的原始实现而不是我们自己的包装。
void *lookup_symbol(const char *name);

/// 取本模块在内存中的地址区间; 失败返回 false。
bool self_module_address(uintptr_t *start, uintptr_t *end);

// ------------------------------------------------------------ maps 解析 -----
struct MapEntry {
    uintptr_t start;
    uintptr_t end;
    uintptr_t offset;
    dev_t dev;
    ino_t inode;
    const char *path;  // 指向调用期间有效的临时缓冲区, visit 返回后即失效
};

struct MapVisitor {
    virtual ~MapVisitor() = default;
    /// 返回 false 可提前结束遍历。
    virtual bool visit(const MapEntry &e) = 0;
};

/// 解析 /proc/self/maps, 对每个可执行映射调用 visitor。
bool foreach_map(MapVisitor &visitor);

/// 定位某个已加载模块(按路径后缀精确匹配)的 dev/inode 与基址。
bool find_module(const char *name_suffix, dev_t *dev, ino_t *inode, uintptr_t *base);

// ------------------------------------------------------------ 时间/随机 -----
uint64_t monotonic_ns();
uint32_t random_u32();

// --------------------------------------------------------------- 基址标记----
// 本模块内的一个实体函数, 供 dladdr 定位模块自身地址区间。
void module_address_marker();

} // namespace hw80
