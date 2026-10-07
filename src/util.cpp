#include "util.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>

namespace hw80 {

void module_address_marker() {}

// ---------------------------------------------------------------------------
// 内部文件 IO: 一律走 syscall, 绕过被 hook 的 libc 包装
// ---------------------------------------------------------------------------
#ifndef SYS_openat
#if defined(__aarch64__)
#define SYS_openat 56
#elif defined(__arm__)
#define SYS_openat 322
#elif defined(__x86_64__)
#define SYS_openat 257
#else
#error "unsupported architecture"
#endif
#endif

int sys_open_read(const char *path) {
    return static_cast<int>(::syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0));
}

ssize_t sys_read(int fd, void *buf, size_t count) {
    return static_cast<ssize_t>(::syscall(SYS_read, fd, buf, count));
}

int sys_close(int fd) { return static_cast<int>(::syscall(SYS_close, fd)); }

long sys_read_file(const char *path, char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    int fd = sys_open_read(path);
    if (fd < 0) return -1;
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = sys_read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
    sys_close(fd);
    buf[total] = '\0';
    return static_cast<long>(total);
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
namespace {

using LogFn = int (*)(int prio, const char *tag, const char *fmt, ...);

LogFn g_log = nullptr;
bool g_debug = false;
bool g_log_tried = false;

// 自己实现的极简格式化: 不依赖 libc 的 vfprintf, 也不用堆。
void append_uint(char *&out, const char *end, unsigned long long v, int base, int width,
                 char pad) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < 24) {
            unsigned d = static_cast<unsigned>(v % static_cast<unsigned>(base));
            tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + (d - 10));
            v /= static_cast<unsigned>(base);
        }
    }
    while (n < width && n < 24) tmp[n++] = pad;
    while (n > 0 && out < end) *out++ = tmp[--n];
}

void append_int(char *&out, const char *end, long long v, int width, char pad) {
    if (v < 0) {
        if (out < end) *out++ = '-';
        append_uint(out, end, static_cast<unsigned long long>(-v), 10, width, pad);
    } else {
        append_uint(out, end, static_cast<unsigned long long>(v), 10, width, pad);
    }
}

const char *format_one(char *&out, const char *end, const char *fmt, va_list &ap) {
    while (*fmt && *fmt != '%' && out < end) *out++ = *fmt++;
    if (*fmt == '\0') return fmt;
    ++fmt;  // 跳过 '%'

    bool left = false;
    char pad = ' ';
    for (;;) {
        if (*fmt == '-') { left = true; ++fmt; continue; }
        if (*fmt == '0') { pad = '0'; ++fmt; continue; }
        break;
    }
    int width = 0;
    while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); ++fmt; }
    while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' || *fmt == 'j' || *fmt == 't') ++fmt;

    char tmp[8];
    const char *src = nullptr;
    int len = 0;
    char numbuf[24];

    switch (*fmt) {
        case 's': {
            src = va_arg(ap, const char *);
            if (!src) src = "(null)";
            while (src[len]) ++len;
            break;
        }
        case 'd':
        case 'i': {
            char *p = numbuf;
            const char *pe = numbuf + sizeof(numbuf);
            append_int(p, pe, va_arg(ap, int), 0, ' ');
            src = numbuf;
            len = static_cast<int>(p - numbuf);
            break;
        }
        case 'u': {
            char *p = numbuf;
            const char *pe = numbuf + sizeof(numbuf);
            append_uint(p, pe, va_arg(ap, unsigned), 10, 0, ' ');
            src = numbuf;
            len = static_cast<int>(p - numbuf);
            break;
        }
        case 'x':
        case 'X':
        case 'p': {
            unsigned long long v;
            if (*fmt == 'p')
                v = reinterpret_cast<unsigned long long>(va_arg(ap, void *));
            else
                v = va_arg(ap, unsigned);
            char *p = numbuf;
            const char *pe = numbuf + sizeof(numbuf);
            if (*fmt == 'p') { *p++ = '0'; *p++ = 'x'; }
            append_uint(p, pe, v, 16, 0, ' ');
            src = numbuf;
            len = static_cast<int>(p - numbuf);
            break;
        }
        case 'c': {
            tmp[0] = static_cast<char>(va_arg(ap, int));
            src = tmp;
            len = 1;
            break;
        }
        case '%': {
            if (out < end) *out++ = '%';
            return fmt + 1;
        }
        default:
            if (out < end) *out++ = '%';
            if (out < end) *out++ = *fmt;
            return fmt + 1;
    }

    int pads = width > len ? width - len : 0;
    if (!left)
        for (int i = 0; i < pads && out < end; ++i) *out++ = pad;
    for (int i = 0; i < len && out < end; ++i) *out++ = src[i];
    if (left)
        for (int i = 0; i < pads && out < end; ++i) *out++ = pad;
    return fmt + 1;
}

} // namespace

void log_init() {
    if (g_log_tried) return;
    g_log_tried = true;
    // __android_log_print 属于 liblog, 由 libc 的 DT_NEEDED 或者目标进程自带;
    // lookup_symbol 会跳过本模块自身的同名导出。
    g_log = reinterpret_cast<LogFn>(lookup_symbol("__android_log_print"));
}

void log_set_debug(bool on) { g_debug = on; }
bool log_debug_enabled() { return g_debug; }

void log_printf(int prio, const char *fmt, ...) {
    if (!g_log) {
        log_init();
        if (!g_log) return;
    }
    char buf[1024];
    char *out = buf;
    const char *end = buf + sizeof(buf) - 1;

    va_list ap;
    va_start(ap, fmt);
    const char *p = fmt;
    while (*p && out < end) p = format_one(out, end, p, ap);
    va_end(ap);
    *out = '\0';

    g_log(prio, "HW80Pro", "%s", buf);
}

// ---------------------------------------------------------------------------
// 字符串 / 内存
// ---------------------------------------------------------------------------
size_t str_len(const char *s) { return s ? __builtin_strlen(s) : 0; }

bool str_eq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

bool str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

bool str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    const size_t sl = str_len(s), fl = str_len(suffix);
    if (fl > sl) return false;
    const char *tail = s + (sl - fl);
    for (size_t i = 0; i < fl; ++i)
        if (tail[i] != suffix[i]) return false;
    return true;
}

bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    if (!*needle) return true;
    for (; *haystack; ++haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { ++h; ++n; }
        if (!*n) return true;
    }
    return false;
}

void *mem_copy(void *dst, const void *src, size_t n) {
    return __builtin_memcpy(dst, src, n);
}

void *mem_set(void *dst, int c, size_t n) { return __builtin_memset(dst, c, n); }

size_t copy_to_buf(char *dst, size_t cap, const char *src, size_t src_len) {
    if (!dst || cap == 0) return 0;
    size_t n = src_len < cap - 1 ? src_len : cap - 1;
    if (src && n) mem_copy(dst, src, n);
    dst[n] = '\0';
    return n;
}

size_t copy_to_buf(char *dst, size_t cap, const char *src) {
    return copy_to_buf(dst, cap, src, src ? str_len(src) : 0);
}

long parse_long(const char *s, long fallback) {
    if (!s) return fallback;
    while (*s == ' ' || *s == '\t') ++s;
    bool neg = false;
    if (*s == '-') { neg = true; ++s; } else if (*s == '+') { ++s; }
    if (*s < '0' || *s > '9') return fallback;
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

unsigned long parse_hex(const char *s, unsigned long fallback) {
    if (!s) return fallback;
    while (*s == ' ' || *s == '\t') ++s;
    unsigned long v = 0;
    bool any = false;
    for (;;) {
        const char c = *s;
        unsigned d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
        else break;
        v = v * 16 + d;
        any = true;
        ++s;
    }
    return any ? v : fallback;
}

bool parse_bool(const char *s) {
    if (!s) return false;
    if (str_eq(s, "1") || str_eq(s, "true") || str_eq(s, "TRUE") || str_eq(s, "yes") ||
        str_eq(s, "on"))
        return true;
    return parse_long(s, 0) != 0;
}

char *trim(char *s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    char *end = s + str_len(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        --end;
    *end = '\0';
    return s;
}

// ---------------------------------------------------------------------------
// 符号解析
// ---------------------------------------------------------------------------
bool self_module_address(uintptr_t *start, uintptr_t *end) {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&module_address_marker), &info) || !info.dli_fbase)
        return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    if (start) *start = base;
    if (end) *end = base + (64u << 20);  // 足够覆盖本模块的保守上界
    return true;
}

namespace {
inline bool is_self(void *p, uintptr_t s, uintptr_t e, bool have_self) {
    if (!have_self || !p) return false;
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= s && v < e;
}
} // namespace

void *lookup_symbol(const char *name) {
    uintptr_t s = 0, e = 0;
    const bool have_self = self_module_address(&s, &e);

    // RTLD_DEFAULT 按加载顺序查找, 会先命中本模块导出的同名符号(例如我们导出的
    // open), 所以必须跳过落在本模块地址区间内的结果。
    if (void *p = dlsym(RTLD_DEFAULT, name); p && !is_self(p, s, e, have_self))
        return p;

    if (void *p = dlsym(RTLD_NEXT, name); p && !is_self(p, s, e, have_self))
        return p;

    // 兜底: 逐个常见库查找
    static const char *const kLibs[] = {
        "libc.so", "liblog.so", "libdl.so", "libandroid.so", "libart.so",
        "libandroid_runtime.so", nullptr,
    };
    for (int i = 0; kLibs[i]; ++i) {
        void *h = dlopen(kLibs[i], RTLD_NOW | RTLD_NOLOAD);
        if (!h) continue;
        if (void *p = dlsym(h, name); p && !is_self(p, s, e, have_self)) return p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// /proc/self/maps 解析
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kMapPathMax = 4096;

class MapParser {
public:
    explicit MapParser(MapVisitor &v) : visitor_(v) {}

    void feed(const char *buf, size_t len, bool &stop) {
        for (size_t i = 0; i < len && !stop; ++i) {
            const char c = buf[i];
            if (c == '\n') {
                line_[line_len_] = '\0';
                if (line_len_ > 0) stop = !parse_line();
                line_len_ = 0;
            } else if (line_len_ + 1 < sizeof(line_)) {
                line_[line_len_++] = c;
            }
        }
    }

    void flush(bool &stop) {
        if (line_len_ > 0 && !stop) {
            line_[line_len_] = '\0';
            stop = !parse_line();
            line_len_ = 0;
        }
    }

private:
    bool parse_line() {
        // 7f8a1c0000-7f8a1c2000 r-xp 00000000 fd:01 1234 /system/lib64/libc.so
        char *p = line_;
        MapEntry e{};

        e.start = parse_hex(p, 0);
        while (*p && *p != '-') ++p;
        if (*p == '-') ++p;
        e.end = parse_hex(p, 0);
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;

        const bool exec = (p[0] != '\0' && p[1] != '\0' && p[2] == 'x');
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;

        e.offset = parse_hex(p, 0);
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;

        // device: major(hex):minor(hex), 合成一个稳定的 64 位键值
        const unsigned long major = parse_hex(p, 0);
        while (*p && *p != ':') ++p;
        if (*p == ':') ++p;
        const unsigned long minor = parse_hex(p, 0);
        e.dev = static_cast<dev_t>((major << 20) | (minor & 0xfffff));
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;

        e.inode = static_cast<ino_t>(parse_hex(p, 0));
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;

        e.path = nullptr;
        if (*p) {
            const size_t n = str_len(p);
            if (n < kMapPathMax) {
                mem_copy(scratch_, p, n + 1);
                e.path = scratch_;
            }
        }

        if (!exec) return true;
        return visitor_.visit(e);
    }

    MapVisitor &visitor_;
    char line_[1024]{};
    size_t line_len_ = 0;
    char scratch_[kMapPathMax]{};
};

struct ModuleFinder : MapVisitor {
    const char *suffix = nullptr;
    dev_t *dev = nullptr;
    ino_t *inode = nullptr;
    uintptr_t *base = nullptr;
    bool found = false;

    bool visit(const MapEntry &e) override {
        if (!e.path || !str_ends_with(e.path, suffix)) return true;
        if (dev) *dev = e.dev;
        if (inode) *inode = e.inode;
        if (base && *base == 0) *base = e.start;
        found = true;
        return false;
    }
};

} // namespace

bool foreach_map(MapVisitor &visitor) {
    const int fd = sys_open_read("/proc/self/maps");
    if (fd < 0) return false;

    MapParser parser(visitor);
    char buf[8192];
    bool stop = false;
    while (!stop) {
        const ssize_t n = sys_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        parser.feed(buf, static_cast<size_t>(n), stop);
    }
    parser.flush(stop);
    sys_close(fd);
    return true;
}

bool find_module(const char *name_suffix, dev_t *dev, ino_t *inode, uintptr_t *base) {
    ModuleFinder f{};
    f.suffix = name_suffix;
    f.dev = dev;
    f.inode = inode;
    f.base = base;
    foreach_map(f);
    return f.found;
}

// ---------------------------------------------------------------------------
// 时间 / 随机
// ---------------------------------------------------------------------------
uint64_t monotonic_ns() {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint32_t random_u32() {
    static uint32_t state = 0;
    if (state == 0) {
        state = static_cast<uint32_t>(monotonic_ns()) ^ 0x9e3779b9u;
        if (state == 0) state = 0x12345678u;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

} // namespace hw80
