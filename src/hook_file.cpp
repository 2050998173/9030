// 虚拟文件内容替换。
//
// 目标: 不管应用用哪种方式读 /proc/cpuinfo, 拿到的都是伪装内容。
//   - open/openat + read/pread64      (Java FileInputStream 走这条)
//   - fopen + fread/fgets             (NDK 代码走这条)
//   - fseek 定位后继续读               (按我们的长度处理)
//
// 做法: 在 open/openat/fopen 时判断路径是否命中伪装表, 命中就把 fd 记进一张
// 定长表; 之后 read/pread64/fread/fgets 从内存内容里返回。fclose 时清表项。
//
// 整条路径不使用堆分配、不使用 C++ 容器, 热路径(read)上只有一个原子读。
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.hpp"
#include "hooks.hpp"
#include "plt_hook.hpp"
#include "spoof_db.hpp"
#include "util.hpp"

namespace hw80 {
namespace {

// --- 原始实现 ---
int (*g_orig_open)(const char *path, int flags, ...) = nullptr;
int (*g_orig_open64)(const char *path, int flags, ...) = nullptr;
int (*g_orig_openat)(int dirfd, const char *path, int flags, ...) = nullptr;
int (*g_orig_openat64)(int dirfd, const char *path, int flags, ...) = nullptr;
FILE *(*g_orig_fopen)(const char *path, const char *mode) = nullptr;
FILE *(*g_orig_fopen64)(const char *path, const char *mode) = nullptr;
ssize_t (*g_orig_read)(int fd, void *buf, size_t count) = nullptr;
ssize_t (*g_orig_pread64)(int fd, void *buf, size_t count, off64_t offset) = nullptr;
size_t (*g_orig_fread)(void *ptr, size_t size, size_t nmemb, FILE *stream) = nullptr;
char *(*g_orig_fgets)(char *s, int size, FILE *stream) = nullptr;
int (*g_orig_fseek)(FILE *stream, long offset, int whence) = nullptr;
int (*g_orig_fclose)(FILE *stream) = nullptr;

// ---------------------------------------------------------------------------
// fd 表
// ---------------------------------------------------------------------------
constexpr int kEmptyFd = -1;
constexpr size_t kMaxFdOverrides = 64;

struct FdEntry {
    int fd = kEmptyFd;
    const char *data = nullptr;
    size_t size = 0;
    size_t offset = 0;
    FILE *stream = nullptr;  // 如果是通过 fopen 打开的, 记住 FILE*
};

FdEntry g_fd_table[kMaxFdOverrides];
int g_override_refs = 0;  // 原子: 非 0 时才需要在 read 里查表

/// 命中伪装表则返回条目, 否则 nullptr。
const TextEntry *match_override(const char *path) {
    if (!path || path[0] != '/') return nullptr;

    const TextEntry *entries = SpoofDb::instance().text_entries();
    const size_t count = SpoofDb::instance().text_count();

    for (size_t i = 0; i < count; ++i) {
        const TextEntry &e = entries[i];
        if (!e.path) continue;

        if (str_eq(e.path, path)) return &e;

        if (e.suffix && str_ends_with(path, e.suffix)) {
            // 防误伤: /sys/.../cpufreq/cpuinfo_max_freq 也以 "cpuinfo" 结尾
            if (str_contains(path, "cpufreq")) continue;
            return &e;
        }
    }
    return nullptr;
}

int insert_fd(int fd, const TextEntry &entry, FILE *stream) {
    for (size_t i = 0; i < kMaxFdOverrides; ++i) {
        if (g_fd_table[i].fd == fd) {  // fd 被复用, 覆盖旧记录
            g_fd_table[i].data = entry.data;
            g_fd_table[i].size = entry.size;
            g_fd_table[i].offset = 0;
            g_fd_table[i].stream = stream;
            return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < kMaxFdOverrides; ++i) {
        if (g_fd_table[i].fd == kEmptyFd) {
            g_fd_table[i].data = entry.data;
            g_fd_table[i].size = entry.size;
            g_fd_table[i].offset = 0;
            g_fd_table[i].stream = stream;
            __atomic_store_n(&g_fd_table[i].fd, fd, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_override_refs, 1, __ATOMIC_RELEASE);
            return static_cast<int>(i);
        }
    }
    return -1;
}

FdEntry *find_fd(int fd) {
    if (__atomic_load_n(&g_override_refs, __ATOMIC_RELAXED) == 0) return nullptr;
    for (size_t i = 0; i < kMaxFdOverrides; ++i) {
        if (__atomic_load_n(&g_fd_table[i].fd, __ATOMIC_ACQUIRE) == fd) return &g_fd_table[i];
    }
    return nullptr;
}

FdEntry *find_stream(FILE *stream) {
    if (!stream || __atomic_load_n(&g_override_refs, __ATOMIC_RELAXED) == 0) return nullptr;
    for (size_t i = 0; i < kMaxFdOverrides; ++i) {
        if (__atomic_load_n(&g_fd_table[i].fd, __ATOMIC_ACQUIRE) != kEmptyFd &&
            g_fd_table[i].stream == stream)
            return &g_fd_table[i];
    }
    return nullptr;
}

void release_fd_entry(FdEntry *e) {
    if (!e || e->fd == kEmptyFd) return;
    e->stream = nullptr;
    __atomic_store_n(&e->fd, kEmptyFd, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&g_override_refs, 1, __ATOMIC_RELEASE);
}

/// 从伪装内容里取一段。返回实际写入的字节数。
size_t serve_bytes(FdEntry *e, void *buf, size_t count, size_t offset) {
    if (!e || !e->data || !buf) return 0;
    if (offset >= e->size) return 0;
    size_t n = e->size - offset;
    if (n > count) n = count;
    mem_copy(buf, e->data + offset, n);
    return n;
}

// ---------------------------------------------------------------------------
// hook 实现
// ---------------------------------------------------------------------------
int hf_open_common(const char *path, int flags, mode_t mode, int (*orig)(const char *, int, ...),
                   bool has_mode) {
    if (!orig) return -1;
    const int fd = has_mode ? orig(path, flags, mode) : orig(path, flags);
    if (fd < 0) return fd;

    if (const TextEntry *entry = match_override(path)) {
        if (insert_fd(fd, *entry, nullptr) >= 0)
            LOGD("open(%s) -> fd %d, 内容将被替换 (%zu 字节)", path, fd, entry->size);
    }
    return fd;
}

int hf_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    return hf_open_common(path, flags, mode, g_orig_open, (flags & O_CREAT) != 0);
}

int hf_open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    return hf_open_common(path, flags, mode, g_orig_open64, (flags & O_CREAT) != 0);
}

int hf_openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    if (!g_orig_openat) return -1;
    const int fd = (flags & O_CREAT) ? g_orig_openat(dirfd, path, flags, mode)
                                     : g_orig_openat(dirfd, path, flags);
    if (fd >= 0) {
        if (const TextEntry *entry = match_override(path)) {
            if (insert_fd(fd, *entry, nullptr) >= 0)
                LOGD("openat(%s) -> fd %d, 内容将被替换", path, fd);
        }
    }
    return fd;
}

int hf_openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }
    if (!g_orig_openat64) return -1;
    const int fd = (flags & O_CREAT) ? g_orig_openat64(dirfd, path, flags, mode)
                                     : g_orig_openat64(dirfd, path, flags);
    if (fd >= 0) {
        if (const TextEntry *entry = match_override(path)) insert_fd(fd, *entry, nullptr);
    }
    return fd;
}

FILE *hf_fopen(const char *path, const char *mode) {
    if (!g_orig_fopen) return nullptr;
    FILE *fp = g_orig_fopen(path, mode);
    if (!fp) return fp;

    if (const TextEntry *entry = match_override(path)) {
        const int fd = fileno(fp);
        if (fd >= 0 && insert_fd(fd, *entry, fp) >= 0)
            LOGD("fopen(%s) -> FILE*, 内容将被替换", path);
    }
    return fp;
}

FILE *hf_fopen64(const char *path, const char *mode) {
    if (!g_orig_fopen64) return nullptr;
    FILE *fp = g_orig_fopen64(path, mode);
    if (!fp) return fp;

    if (const TextEntry *entry = match_override(path)) {
        const int fd = fileno(fp);
        if (fd >= 0) insert_fd(fd, *entry, fp);
    }
    return fp;
}

ssize_t hf_read(int fd, void *buf, size_t count) {
    FdEntry *e = find_fd(fd);
    if (e) {
        const size_t n = serve_bytes(e, buf, count, e->offset);
        e->offset += n;
        return static_cast<ssize_t>(n);
    }
    return g_orig_read ? g_orig_read(fd, buf, count) : -1;
}

ssize_t hf_pread64(int fd, void *buf, size_t count, off64_t offset) {
    FdEntry *e = find_fd(fd);
    if (e) {
        if (offset < 0) return -1;
        return static_cast<ssize_t>(serve_bytes(e, buf, count, static_cast<size_t>(offset)));
    }
    return g_orig_pread64 ? g_orig_pread64(fd, buf, count, offset) : -1;
}

size_t hf_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    FdEntry *e = find_stream(stream);
    if (e && size > 0 && nmemb > 0) {
        const size_t want = size * nmemb;
        const size_t got = serve_bytes(e, ptr, want, e->offset);
        e->offset += got;
        return got / size;
    }
    return g_orig_fread ? g_orig_fread(ptr, size, nmemb, stream) : 0;
}

char *hf_fgets(char *s, int size, FILE *stream) {
    FdEntry *e = find_stream(stream);
    if (e && s && size > 1) {
        if (e->offset >= e->size) return nullptr;
        // 最多写入 size-1 个字符, 再加结尾 '\0'
        const size_t limit = static_cast<size_t>(size - 1);
        size_t n = 0;
        while (n < limit && e->offset + n < e->size) {
            const char c = e->data[e->offset + n];
            s[n++] = c;
            if (c == '\n') break;
        }
        e->offset += n;
        s[n] = '\0';
        return s;
    }
    return g_orig_fgets ? g_orig_fgets(s, size, stream) : nullptr;
}

int hf_fseek(FILE *stream, long offset, int whence) {
    FdEntry *e = find_stream(stream);
    if (e) {
        switch (whence) {
            case SEEK_SET:
                if (offset < 0) return -1;
                e->offset = static_cast<size_t>(offset);
                return 0;
            case SEEK_END:
                if (offset > 0) return -1;
                e->offset = (e->size >= static_cast<size_t>(-offset))
                                ? e->size - static_cast<size_t>(-offset)
                                : 0;
                return 0;
            case SEEK_CUR:
                if (offset < 0 && static_cast<size_t>(-offset) > e->offset) e->offset = 0;
                else e->offset = static_cast<size_t>(static_cast<long>(e->offset) + offset);
                return 0;
            default:
                break;
        }
    }
    return g_orig_fseek ? g_orig_fseek(stream, offset, whence) : -1;
}

int hf_fclose(FILE *stream) {
    FdEntry *e = find_stream(stream);
    if (e) release_fd_entry(e);
    return g_orig_fclose ? g_orig_fclose(stream) : -1;
}

} // namespace

void hook_install_file_overlay() {
    if (SpoofDb::instance().text_count() == 0) {
        LOGW("伪装文件表为空, 跳过文件内容替换");
        return;
    }
    if (!g_orig_read) {
        LOGW("read 未解析到, 文件内容替换无法工作");
        return;
    }

    plt_hook_add("open", reinterpret_cast<void *>(&hf_open), nullptr);
    plt_hook_add("openat", reinterpret_cast<void *>(&hf_openat), nullptr);
    plt_hook_add("read", reinterpret_cast<void *>(&hf_read), nullptr);
    if (g_orig_open64) plt_hook_add("open64", reinterpret_cast<void *>(&hf_open64), nullptr);
    if (g_orig_openat64) plt_hook_add("openat64", reinterpret_cast<void *>(&hf_openat64), nullptr);

    if (g_orig_fopen) plt_hook_add("fopen", reinterpret_cast<void *>(&hf_fopen), nullptr);
    if (g_orig_fopen64) plt_hook_add("fopen64", reinterpret_cast<void *>(&hf_fopen64), nullptr);
    if (g_orig_fread) plt_hook_add("fread", reinterpret_cast<void *>(&hf_fread), nullptr);
    if (g_orig_fgets) plt_hook_add("fgets", reinterpret_cast<void *>(&hf_fgets), nullptr);
    if (g_orig_fseek) plt_hook_add("fseek", reinterpret_cast<void *>(&hf_fseek), nullptr);
    if (g_orig_fclose) plt_hook_add("fclose", reinterpret_cast<void *>(&hf_fclose), nullptr);
    if (g_orig_pread64) plt_hook_add("pread64", reinterpret_cast<void *>(&hf_pread64), nullptr);

    LOGD("文件替换 hook 已注册, 覆盖 %zu 个路径", SpoofDb::instance().text_count());
}

/// 在 hook_resolve_originals() 里调用, 解析本模块需要的所有原始函数。
void file_overlay_resolve_originals() {
    g_orig_open = reinterpret_cast<decltype(g_orig_open)>(lookup_symbol("open"));
    g_orig_open64 = reinterpret_cast<decltype(g_orig_open64)>(lookup_symbol("open64"));
    g_orig_openat = reinterpret_cast<decltype(g_orig_openat)>(lookup_symbol("openat"));
    g_orig_openat64 = reinterpret_cast<decltype(g_orig_openat64)>(lookup_symbol("openat64"));
    g_orig_fopen = reinterpret_cast<decltype(g_orig_fopen)>(lookup_symbol("fopen"));
    g_orig_fopen64 = reinterpret_cast<decltype(g_orig_fopen64)>(lookup_symbol("fopen64"));
    g_orig_read = reinterpret_cast<decltype(g_orig_read)>(lookup_symbol("read"));
    g_orig_pread64 = reinterpret_cast<decltype(g_orig_pread64)>(lookup_symbol("pread64"));
    g_orig_fread = reinterpret_cast<decltype(g_orig_fread)>(lookup_symbol("fread"));
    g_orig_fgets = reinterpret_cast<decltype(g_orig_fgets)>(lookup_symbol("fgets"));
    g_orig_fseek = reinterpret_cast<decltype(g_orig_fseek)>(lookup_symbol("fseek"));
    g_orig_fclose = reinterpret_cast<decltype(g_orig_fclose)>(lookup_symbol("fclose"));

    LOGD("文件相关原始符号: open=%p read=%p fopen=%p fgets=%p",
         reinterpret_cast<void *>(g_orig_open), reinterpret_cast<void *>(g_orig_read),
         reinterpret_cast<void *>(g_orig_fopen), reinterpret_cast<void *>(g_orig_fgets));
}

} // namespace hw80
