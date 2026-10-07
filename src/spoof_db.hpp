// 伪装数据库: 属性覆盖表 + 虚拟文件覆盖表。
//
// 数据结构是"进程内一次性构建, 之后只读"的, 因此运行期不需要加锁:
// 构建发生在 preAppSpecialize (单线程), 之后只有读取。
// 为了让它在 libc 构造函数跑完之前就可用, 底层存储是 POD 数组 + 原子索引,
// 没有动态初始化。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hw80 {

struct TextEntry {
    const char *path = nullptr;  // 规范化全路径
    const char *suffix = nullptr; // 用于后缀匹配(可为空); 空则只做全路径匹配
    const char *data = nullptr;   // arena 里的内容
    size_t size = 0;              // 不含结尾 '\0'
};

struct PathOverride {
    const char *key = nullptr;
    const char *value = nullptr;
};

struct AppEntry {
    const char *name = nullptr;
};

/// 无锁 bump 分配器。容量固定, 用尽后分配失败(上层退化为"不伪装该条")。
class Arena {
public:
    static constexpr size_t kCapacity = 512u * 1024u;

    char *alloc(size_t bytes);
    /// 复制字符串(含结尾 '\0'), 返回稳定指针; 空间不足返回 nullptr。
    const char *dup(const char *s, size_t len);

private:
    alignas(16) char data_[kCapacity]{};
    uint32_t used_ = 0;
};

class SpoofDb {
public:
    static SpoofDb &instance();

    // ---- 构建期(preAppSpecialize, 单线程) ----
    Arena &arena() { return arena_; }

    bool add_property(const char *name, const char *value, size_t value_len);
    bool add_text(const char *path, const char *suffix, const char *data, size_t size);
    bool add_app(const char *name);
    void set_mem_total_kb(unsigned long kb) { mem_total_kb_ = kb; }
    void set_cpuinfo_source_file(const char *path) { cpuinfo_file_ = path; }

    // ---- 运行期(只读) ----
    const char *get_property(const char *name, size_t *out_len) const;
    /// 按下标取属性(hook_property 预生成伪造 prop_info 时需要遍历)。
    /// 下标越界返回 false。
    bool property_at(size_t index, const char **name, const char **value, size_t *value_len) const;
    const TextEntry *text_entries() const { return text_; }
    size_t text_count() const { return text_count_; }
    const char *const *apps() const { return app_names_; }
    size_t app_count() const { return app_count_; }
    unsigned long mem_total_kb() const { return mem_total_kb_; }
    const char *cpuinfo_file() const { return cpuinfo_file_; }
    size_t property_count() const { return prop_count_; }

    /// 判断包名 / 进程名是否在伪装范围内。
    /// 支持 "com.foo.bar" 与 "com.foo.bar:process" 两种写法。
    bool app_matches(const char *process_name) const;

private:
    SpoofDb() = default;

    // 属性表: 固定容量 + 线性查找(条目数为数百, 命中时开销可忽略)
    static constexpr size_t kMaxProps = 512;
    const char *prop_names_[kMaxProps]{};
    const char *prop_values_[kMaxProps]{};
    uint32_t prop_lens_[kMaxProps]{};
    uint32_t prop_count_ = 0;

    static constexpr size_t kMaxTexts = 128;
    TextEntry text_[kMaxTexts]{};
    uint32_t text_count_ = 0;

    static constexpr size_t kMaxApps = 64;
    const char *app_names_[kMaxApps]{};
    uint32_t app_count_ = 0;

    unsigned long mem_total_kb_ = 0;
    const char *cpuinfo_file_ = nullptr;
    Arena arena_{};
};

// ------------------------------------------------------------- 路径常量 -----
// 注意: 文件覆盖表里存的正是下面这些规范化路径。
namespace paths {
inline constexpr const char *kProcCpuInfo = "/proc/cpuinfo";
inline constexpr const char *kProcMemInfo = "/proc/meminfo";
inline constexpr const char *kSysCpuPossible = "/sys/devices/system/cpu/possible";
inline constexpr const char *kSysCpuPresent = "/sys/devices/system/cpu/present";
inline constexpr const char *kSysCpuOnline = "/sys/devices/system/cpu/online";
} // namespace paths

} // namespace hw80
