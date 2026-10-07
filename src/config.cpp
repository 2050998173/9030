#include "config.hpp"

#include "profile_data.hpp"
#include "spoof_db.hpp"
#include "util.hpp"

namespace hw80 {

namespace {

// 与 config/proc_cpuinfo.txt 末尾对应的设备表头。内置模板只放 9 个 processor 块,
// 表头在这里补上, 这样配置文件里可以只维护 processor 部分。
constexpr const char *kEmbeddedCpuInfoHead =
    "\n"
    "Hardware\t: HUAWEI Kirin 9030 Pro\n"
    "Revision\t: 0000\n"
    "Serial\t\t: 0000000000000000\n";

constexpr unsigned int kSpoofCoreCount = 9;  // 麒麟 9030 Pro: 9 核 14 线程

Options g_options{};
UnameConfig g_uname{};
bool g_loaded = false;
bool g_cpuinfo_disabled = false;
bool g_cpuinfo_is_device = false;
const char *g_cpuinfo_file = nullptr;
const char *g_cpuinfo_text = nullptr;  // 外部传入或编译内置的模板
size_t g_cpuinfo_text_len = 0;
unsigned long g_mem_total_kb = 0;

// --------------------------------------------------------------- 小工具 -----
/// 在 [begin, end) 内找子串, 返回其在 begin 中的下标; 找不到返回 npos。
size_t find_sub(const char *begin, const char *end, const char *needle) {
    const size_t nl = str_len(needle);
    if (nl == 0) return 0;
    if (static_cast<size_t>(end - begin) < nl) return static_cast<size_t>(-1);
    for (const char *p = begin; p + nl <= end; ++p) {
        size_t i = 0;
        while (i < nl && p[i] == needle[i]) ++i;
        if (i == nl) return static_cast<size_t>(p - begin);
    }
    return static_cast<size_t>(-1);
}

bool key_in_set(const char *key, const char *const *set) {
    for (int i = 0; set[i]; ++i)
        if (str_eq(key, set[i])) return true;
    return false;
}

// ------------------------------------------------------------- cpuinfo -----
/// 拷贝模板内容, 顺手丢掉以 '#' 开头的注释行。
/// 真实内核的 /proc/cpuinfo 里不会出现注释, 如果原样送出去, 一眼就能看出是
/// 伪造的 —— 那会让整个模块失去意义。所以这里做一次过滤。
void append_without_comments(const char *in, size_t in_len, char *out, size_t out_cap,
                             size_t &out_len) {
    const char *p = in;
    const char *const end = in + in_len;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') ++nl;
        const size_t line_len = static_cast<size_t>(nl - p);

        const bool is_comment = (line_len > 0 && *p == '#');
        if (!is_comment && out_len + line_len + 1 < out_cap) {
            if (line_len) mem_copy(out + out_len, p, line_len);
            out_len += line_len;
            out[out_len++] = '\n';
        }
        p = (nl < end) ? nl + 1 : end;
    }
    out[out_len] = '\0';
}

/// 把内容(不含表头)与设备表头拼成完整 cpuinfo 并注册。
void install_cpuinfo(const char *body, size_t body_len) {
    Arena &arena = SpoofDb::instance().arena();
    const size_t head_len = str_len(kEmbeddedCpuInfoHead);
    const size_t cap = body_len + head_len + 2;
    char *buf = arena.alloc(cap);
    if (!buf) {
        LOGW("arena 空间不足, 无法安装 cpuinfo 模板");
        return;
    }
    size_t used = 0;
    append_without_comments(body, body_len, buf, cap, used);

    // 表头也去重: 模板里已经带了 Hardware/Revision 的话就不再追加,
    // 否则伪装出来的 cpuinfo 会出现两遍 Hardware 行。
    const bool has_hardware = str_contains(buf, "\nHardware") || str_starts_with(buf, "Hardware");
    if (!has_hardware && head_len && used + head_len + 1 <= cap) {
        mem_copy(buf + used, kEmbeddedCpuInfoHead, head_len);
        used += head_len;
        buf[used] = '\0';
    }

    if (SpoofDb::instance().add_text(paths::kProcCpuInfo, "/cpuinfo", buf, used))
        LOGD("已安装 cpuinfo 模板 (%zu 字节)", used);
    else
        LOGW("注册 /proc/cpuinfo 覆盖失败");
}

/// cpuinfo=@device: 现读真实内容, 只改厂商/型号/核数痕迹。
void install_cpuinfo_from_device() {
    constexpr size_t kCap = 32 * 1024;
    Arena &arena = SpoofDb::instance().arena();
    char *raw = arena.alloc(kCap);
    if (!raw) return;

    const long n = sys_read_file(paths::kProcCpuInfo, raw, kCap);
    if (n <= 0) {
        LOGW("读取真机 /proc/cpuinfo 失败, 回退到内置模板");
        install_cpuinfo(g_cpuinfo_text, g_cpuinfo_text_len);
        return;
    }

    char *out = arena.alloc(kCap * 2);
    if (!out) return;
    const size_t out_len = cpuinfo_mask_device(raw, static_cast<size_t>(n), out, kCap * 2);
    if (out_len == 0) {
        install_cpuinfo(g_cpuinfo_text, g_cpuinfo_text_len);
        return;
    }
    if (SpoofDb::instance().add_text(paths::kProcCpuInfo, "/cpuinfo", out, out_len))
        LOGD("已按真机 cpuinfo 生成伪装内容 (%zu 字节)", out_len);
    else
        LOGW("注册 /proc/cpuinfo 覆盖失败");
}

void install_cpuinfo_from_file(const char *path) {
    if (!path || !*path) {
        install_cpuinfo(g_cpuinfo_text, g_cpuinfo_text_len);
        return;
    }
    constexpr size_t kCap = 64 * 1024;
    char *buf = SpoofDb::instance().arena().alloc(kCap);
    if (!buf) return;

    const long n = sys_read_file(path, buf, kCap);
    if (n <= 0) {
        LOGW("读取 %s 失败, 回退到内置模板", path);
        install_cpuinfo(g_cpuinfo_text, g_cpuinfo_text_len);
        return;
    }
    install_cpuinfo(buf, static_cast<size_t>(n));
}

// -------------------------------------------------------------- meminfo -----
/// 只替换 MemTotal 行, 其余保持真实。
void install_meminfo(unsigned long mem_total_kb) {
    constexpr size_t kCap = 64 * 1024;
    Arena &arena = SpoofDb::instance().arena();
    char *raw = arena.alloc(kCap);
    if (!raw) return;
    const long n = sys_read_file(paths::kProcMemInfo, raw, kCap);
    if (n <= 0) {
        LOGW("读取 /proc/meminfo 失败, 跳过 MemTotal 伪装");
        return;
    }

    const size_t out_cap = static_cast<size_t>(n) + 128;
    char *out = arena.alloc(out_cap);
    if (!out) return;

    // 组装替换行 "MemTotal:       15872000 kB\n"
    char replacement[64];
    char *w = replacement;
    const char *const wend = replacement + sizeof(replacement) - 1;
    for (const char *p = "MemTotal:       "; *p && w < wend; ++p) *w++ = *p;
    {
        char digits[24];
        int dn = 0;
        unsigned long v = mem_total_kb;
        if (v == 0) digits[dn++] = '0';
        while (v > 0 && dn < 24) { digits[dn++] = static_cast<char>('0' + (v % 10)); v /= 10; }
        while (dn > 0 && w < wend) *w++ = digits[--dn];
    }
    for (const char *p = " kB\n"; *p && w < wend; ++p) *w++ = *p;
    *w = '\0';
    const size_t repl_len = static_cast<size_t>(w - replacement);

    size_t out_len = 0;
    const char *p = raw;
    const char *const end = raw + n;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') ++nl;
        const size_t line_len = static_cast<size_t>(nl - p);

        if (line_len >= 9 && find_sub(p, p + 9, "MemTotal:") == 0) {
            if (out_len + repl_len < out_cap) {
                mem_copy(out + out_len, replacement, repl_len);
                out_len += repl_len;
            }
        } else if (out_len + line_len + 1 < out_cap) {
            mem_copy(out + out_len, p, line_len);
            out_len += line_len;
            out[out_len++] = '\n';
        }
        p = (nl < end) ? nl + 1 : end;
    }
    out[out_len] = '\0';

    if (SpoofDb::instance().add_text(paths::kProcMemInfo, nullptr, out, out_len))
        LOGD("已伪装 MemTotal=%lu kB", mem_total_kb);
    else
        LOGW("注册 /proc/meminfo 覆盖失败");
}

// --------------------------------------------------------------- sysfs ------
/// 只覆盖"核数"相关的 sysfs 文件。频率文件保持真实 —— 那是实时调度信息,
/// 伪造反而容易与真实负载对不上。
void install_sys_cpu(unsigned int cores) {
    char value[16];
    char *w = value;
    const char *const wend = value + sizeof(value) - 1;
    *w++ = '0';
    *w++ = '-';
    {
        char digits[8];
        int dn = 0;
        unsigned int v = cores > 0 ? cores - 1 : 0;
        if (v == 0) digits[dn++] = '0';
        while (v > 0 && dn < 8) { digits[dn++] = static_cast<char>('0' + (v % 10)); v /= 10; }
        while (dn > 0 && w < wend) *w++ = digits[--dn];
    }
    *w = '\0';
    const size_t vlen = str_len(value);

    SpoofDb &db = SpoofDb::instance();
    db.add_text("/sys/devices/system/cpu/possible", "cpu/possible", value, vlen);
    db.add_text("/sys/devices/system/cpu/present", "cpu/present", value, vlen);
    db.add_text("/sys/devices/system/cpu/online", "cpu/online", value, vlen);
    db.add_text("/sys/devices/system/cpu/offline", "cpu/offline", "", 0);
    db.add_text("/sys/devices/system/cpu/kernel_max", "cpu/kernel_max", "8", 1);
    LOGD("已伪装 CPU 核数视图: %s", value);
}

// ---------------------------------------------------------------- 属性 ------
// 模块自身的开关, 不当做系统属性伪装。
// uname_* 在这里, 因为它们的键名不含 '.', 不能作为系统属性, 由 UnameConfig 保存。
const char *const kMetaKeys[] = {
    "apps", "cpuinfo", "meminfo_memtotal_kb", "debug", "hook_property_find",
    "hook_exec_getprop", "patch_sys_cpu", "spoof_uname", "uname_sysname", "uname_nodename",
    "uname_release", "uname_version", "uname_machine", "uname_domain", nullptr,
};

void handle_meta(const char *key, const char *value) {
    UnameConfig &un = uname_config();
    if (str_eq(key, "debug")) {
        g_options.debug = parse_bool(value);
    } else if (str_eq(key, "hook_property_find")) {
        g_options.hook_property_find = parse_bool(value);
    } else if (str_eq(key, "hook_exec_getprop")) {
        g_options.hook_exec_getprop = parse_bool(value);
    } else if (str_eq(key, "patch_sys_cpu")) {
        g_options.patch_sys_cpu = parse_bool(value);
    } else if (str_eq(key, "spoof_uname")) {
        g_options.spoof_uname = parse_bool(value);
    } else if (str_eq(key, "uname_sysname")) {
        if (*value) copy_to_buf(un.sysname, sizeof(un.sysname), value);
    } else if (str_eq(key, "uname_nodename")) {
        if (*value) copy_to_buf(un.nodename, sizeof(un.nodename), value);
    } else if (str_eq(key, "uname_release")) {
        if (*value) copy_to_buf(un.release, sizeof(un.release), value);
    } else if (str_eq(key, "uname_version")) {
        if (*value) copy_to_buf(un.version, sizeof(un.version), value);
    } else if (str_eq(key, "uname_machine")) {
        if (*value) copy_to_buf(un.machine, sizeof(un.machine), value);
    } else if (str_eq(key, "uname_domain")) {
        copy_to_buf(un.domainname, sizeof(un.domainname), value);
    } else if (str_eq(key, "apps")) {
        if (*value) SpoofDb::instance().add_app(value);
    } else if (str_eq(key, "meminfo_memtotal_kb")) {
        if (*value) {
            g_mem_total_kb = static_cast<unsigned long>(parse_long(value, 0));
            g_options.have_mem_total = g_mem_total_kb > 0;
        }
    } else if (str_eq(key, "cpuinfo")) {
        if (str_eq(value, "@none")) {
            g_cpuinfo_disabled = true;
        } else if (str_eq(value, "@device")) {
            g_cpuinfo_is_device = true;
        } else if (str_eq(value, "@file") || !*value) {
            g_cpuinfo_file = paths::kDefaultCpuInfoPath;
        } else {
            const char *path = (value[0] == '@') ? value + 1 : value;
            g_cpuinfo_file = SpoofDb::instance().arena().dup(path, str_len(path));
        }
    }
    // uname_* 由 hook_misc 直接从伪装属性表里读
}

/// 逐行解析。把 line 交给回调前会做续行合并与引号处理。
template <typename Fn>
void for_each_logical_line(const char *text, size_t len, Fn &&fn) {
    char buf[2048];
    size_t used = 0;
    bool truncated = false;

    for (size_t i = 0; i <= len; ++i) {
        const char c = (i < len) ? text[i] : '\n';
        if (c == '\r') continue;
        if (c != '\n') {
            if (used + 1 < sizeof(buf)) {
                buf[used++] = c;
            } else {
                truncated = true;
            }
            continue;
        }

        // 行尾反斜杠 => 与下一行拼接
        if (used > 0 && buf[used - 1] == '\\') {
            --used;
            continue;
        }
        if (truncated) LOGW("配置行过长已截断");
        truncated = false;
        buf[used] = '\0';
        if (used > 0) fn(buf);
        used = 0;
    }
}

void parse_one_line(char *line) {
    char *s = trim(line);
    if (*s == '\0' || *s == '#' || *s == ';') return;

    char *eq = s;
    while (*eq && *eq != '=') ++eq;
    if (*eq != '=') return;

    *eq = '\0';
    const char *key = trim(s);
    char *raw_value = trim(eq + 1);
    if (!*key) return;

    size_t vlen = str_len(raw_value);
    const char *value = raw_value;
    if (vlen >= 2 && ((value[0] == '"' && value[vlen - 1] == '"') ||
                      (value[0] == '\'' && value[vlen - 1] == '\''))) {
        ++value;
        vlen -= 2;
    }

    if (key_in_set(key, kMetaKeys)) {
        // handle_meta 需要以 '\0' 结尾的值
        char tmp[1024];
        copy_to_buf(tmp, sizeof(tmp), value, vlen);
        handle_meta(key, tmp);
        return;
    }

    // 属性名校验: 只允许 [A-Za-z0-9._-], 且必须含 '.'
    bool ok = str_contains(key, ".");
    for (const char *p = key; ok && *p; ++p) {
        const char c = *p;
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!valid) ok = false;
    }
    if (!ok) {
        LOGW("忽略非法属性名: %s", key);
        return;
    }

    if (vlen == 0) return;  // 空值 = 不伪装

    if (vlen > 91) {
        LOGW("属性 %s 的值超过 91 字节, 已截断", key);
        vlen = 91;
    }
    SpoofDb::instance().add_property(key, value, vlen);
}

} // namespace

Options &options() { return g_options; }
UnameConfig &uname_config() { return g_uname; }
bool config_loaded() { return g_loaded; }

void config_parse(const char *text, size_t len, const char *cpuinfo_text,
                  size_t cpuinfo_text_len) {
    if (!text || len == 0) return;

    // 优先用外部传进来的模板(companion 读的文件), 否则用编译内置的
    if (cpuinfo_text && cpuinfo_text_len > 0) {
        g_cpuinfo_text = cpuinfo_text;
        g_cpuinfo_text_len = cpuinfo_text_len;
    } else {
        g_cpuinfo_text = kDefaultCpuInfo.data();
        g_cpuinfo_text_len = kDefaultCpuInfo.size();
    }

    // 第一遍: 属性 + 开关
    for_each_logical_line(text, len, [](char *line) { parse_one_line(line); });

    // 第二遍: 安装虚拟文件
    if (!g_cpuinfo_disabled) {
        if (g_cpuinfo_is_device)
            install_cpuinfo_from_device();
        else
            install_cpuinfo_from_file(g_cpuinfo_file);
    }
    if (g_options.have_mem_total) install_meminfo(g_mem_total_kb);
    if (g_options.patch_sys_cpu) install_sys_cpu(kSpoofCoreCount);

    log_set_debug(g_options.debug);
    g_loaded = true;

    LOGI("配置加载完成: %zu 条属性 / %zu 个应用 / cpuinfo=%s / meminfo=%s",
         SpoofDb::instance().property_count(), SpoofDb::instance().app_count(),
         g_cpuinfo_disabled ? "@none"
                            : (g_cpuinfo_is_device ? "@device"
                                                   : (g_cpuinfo_file ? g_cpuinfo_file : "@file")),
         g_options.have_mem_total ? "on" : "off");
}

size_t cpuinfo_mask_device(const char *in, size_t in_len, char *out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return 0;

    size_t out_len = 0;
    unsigned int index = 0;
    const char *p = in;
    const char *const end = in + in_len;

    auto emit = [&](const char *s, size_t n) -> bool {
        if (out_len + n >= out_cap) return false;
        mem_copy(out + out_len, s, n);
        out_len += n;
        return true;
    };
    auto emit_str = [&](const char *s) { return emit(s, str_len(s)); };

    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') ++nl;
        const size_t line_len = static_cast<size_t>(nl - p);

        if (line_len >= 10 && find_sub(p, p + 10, "processor") == 0) {
            // processor\t: N -> 重新编号
            char buf[64];
            char *w = buf;
            const char *const wend = buf + sizeof(buf) - 1;
            for (const char *s = "processor\t: "; *s && w < wend; ++s) *w++ = *s;
            {
                char digits[8];
                int dn = 0;
                unsigned int v = index;
                if (v == 0) digits[dn++] = '0';
                while (v > 0 && dn < 8) {
                    digits[dn++] = static_cast<char>('0' + (v % 10));
                    v /= 10;
                }
                while (dn > 0 && w < wend) *w++ = digits[--dn];
            }
            *w = '\0';
            if (!emit_str(buf)) break;
            ++index;
        } else if (line_len >= 8 && find_sub(p, p + 8, "Hardware") == 0) {
            if (!emit_str("Hardware\t: HUAWEI Kirin 9030 Pro")) break;
        } else if (line_len >= 8 && find_sub(p, p + 8, "Revision") == 0) {
            if (!emit_str("Revision\t: 0000")) break;
        } else if (line_len >= 25 &&
                   find_sub(p, p + 25, "CPU implementer") == 0) {
            if (!emit_str("CPU implementer\t: 0x48")) break;
        } else if (line_len >= 8 && find_sub(p, p + 8, "CPU part") == 0) {
            // 麒麟 9030 Pro: 1 超大核 + 4 大核 = 0xd84, 4 小核 = 0xd85。
            // index 在处理 "processor" 行时已经自增, 所以这里第 1~5 个块是大核。
            const char *part = (index <= 5) ? "0xd84" : "0xd85";
            if (!emit_str("CPU part\t: ")) break;
            if (!emit_str(part)) break;
        } else {
            // 屏蔽其它厂商的型号名。只认明确的厂商标识, 避免误伤 Features 行。
            const char *const kDrop[] = {
                "SEC_", "SAMSUNG", "Qualcomm", "Snapdragon", "MediaTek", "Dimensity",
                "Spreadtrum", "Exynos", "Unisoc", "AArch64 Processor", "ARMv8",
                "Hardware\t: Qualcomm", "Hardware\t: MT", "Hardware\t: SM", nullptr,
            };
            bool drop = false;
            for (int i = 0; kDrop[i] && !drop; ++i) {
                const size_t il = str_len(kDrop[i]);
                if (il == 0 || il > line_len) continue;
                for (const char *hit = p; hit + il <= p + line_len; ++hit) {
                    size_t k = 0;
                    while (k < il && hit[k] == kDrop[i][k]) ++k;
                    if (k == il) { drop = true; break; }
                }
            }
            if (drop) {
                if (!emit_str("Hardware\t: HUAWEI Kirin 9030 Pro")) break;
            } else if (!emit(p, line_len)) {
                break;
            }
        }

        if (nl < end) {
            if (!emit_str("\n")) break;
            p = nl + 1;
        } else {
            p = end;
        }
    }

    out[out_len] = '\0';
    return out_len;
}

} // namespace hw80
