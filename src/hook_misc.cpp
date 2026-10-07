// 其它 hook: uname 伪装 + getprop 子进程命令行改写。
//
// 两个都是可选项(配置里 spoof_uname / hook_exec_getprop), 默认:
//   uname          —— 关闭。真实内核版本与伪装值差太多反而更容易被识别。
//   getprop 改写   —— 开启。App 常执行 `getprop xxx` 拿值, 那是 fork 出来的
//                     子进程, 我们进程内的属性 hook 管不到, 所以在 execve
//                     之前把参数换掉。
//
// 覆盖两种常见写法:
//   getprop ro.product.model
//   sh -c "getprop ro.product.model"        (Runtime.exec(String) 走这条)
#include <stdarg.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "config.hpp"
#include "hooks.hpp"
#include "plt_hook.hpp"
#include "spoof_db.hpp"
#include "util.hpp"

namespace hw80 {
namespace {

int (*g_orig_uname)(struct utsname *) = nullptr;
int (*g_orig_execve)(const char *, char *const[], char *const[]) = nullptr;
int (*g_orig_execve2)(const char *, char *const[], char *const[]) = nullptr;

// 改写后的参数最多 16 组, 每组的 argv 数组放在这里(保存原指针以便恢复)。
constexpr int kMaxSavedArgvs = 16;
constexpr int kMaxArgvLen = 64;
thread_local char *g_saved_argv[kMaxSavedArgvs][kMaxArgvLen];
thread_local int g_saved_count = 0;

// sh -c 命令串的替换缓冲。同一线程内 execve 是同步的, 而且我们的 hook 在
// execve 返回(失败)后立刻恢复 argv, 所以一个缓冲区就够了。
thread_local char g_cmd_buf[2048];

// ---------------------------------------------------------------------------
// uname
// ---------------------------------------------------------------------------
int hf_uname(struct utsname *buf) {
    const int rc = g_orig_uname ? g_orig_uname(buf) : -1;
    if (rc != 0 || !buf) return rc;

    const UnameConfig &un = uname_config();
    copy_to_buf(buf->sysname, sizeof(buf->sysname), un.sysname);
    copy_to_buf(buf->nodename, sizeof(buf->nodename), un.nodename);
    copy_to_buf(buf->release, sizeof(buf->release), un.release);
    copy_to_buf(buf->version, sizeof(buf->version), un.version);
    copy_to_buf(buf->machine, sizeof(buf->machine), un.machine);
    copy_to_buf(buf->domainname, sizeof(buf->domainname), un.domainname);
    return rc;
}

// ---------------------------------------------------------------------------
// execve: 改写 getprop 的参数
// ---------------------------------------------------------------------------
const char *g_shells[] = {"sh", "bash", "mksh", "ash", "dash", nullptr};

bool is_shell(const char *path) {
    if (!path) return false;
    for (int i = 0; g_shells[i]; ++i) {
        if (str_eq(path, g_shells[i])) return true;
        // /system/bin/sh 之类
        const size_t pl = str_len(path);
        const size_t sl = str_len(g_shells[i]);
        if (pl > sl + 1 && path[pl - sl - 1] == '/' && str_ends_with(path, g_shells[i]))
            return true;
    }
    return false;
}

/// 在 argv 里放一个替换指针, 同时保存原值以便 execve 返回后恢复。
///
/// 注意签名里的 `char *const argv[]` 会调整成 `char *const *`, 于是
/// `argv[i]` 的类型是 `char *const`(指针本身是 const), 不能直接赋值。
/// 这在 execve 的签名里是必须的, 所以这里用 const_cast 拿到可写视图 ——
/// 运行期是安全的, 因为调用方传进来的确实是可写的 argv 数组。
bool push_argv_replacement(char *const argv[], int idx, char *new_value) {
    if (g_saved_count >= kMaxSavedArgvs) return false;
    char **slot = g_saved_argv[g_saved_count];
    int n = 0;
    for (; argv[n] && n < kMaxArgvLen - 1; ++n) slot[n] = argv[n];
    slot[n] = nullptr;
    const_cast<char **>(argv)[idx] = new_value;
    ++g_saved_count;
    return true;
}

void pop_argv_replacement(char *const argv[]) {
    if (g_saved_count <= 0) return;
    --g_saved_count;
    char **slot = g_saved_argv[g_saved_count];
    char **writable = const_cast<char **>(argv);
    for (int n = 0; slot[n] && n < kMaxArgvLen - 1; ++n) writable[n] = slot[n];
}

/// `getprop <key>` / `toybox getprop <key>`: 返回 key 在 argv 中的下标, 否则 -1。
int find_getprop_key(char *const argv[]) {
    if (!argv || !argv[0]) return -1;
    int base = -1;
    if (str_ends_with(argv[0], "getprop") || str_eq(argv[0], "getprop")) {
        base = 0;
    } else if (str_ends_with(argv[0], "toybox") || str_ends_with(argv[0], "toolbox")) {
        if (argv[1] && str_eq(argv[1], "getprop")) base = 1;
    }
    if (base < 0) return -1;

    // getprop            -> 打印全部属性
    // getprop <key>      -> 查一条
    // getprop -h/--help  -> 帮助
    const int key_idx = base + 1;
    if (!argv[key_idx] || argv[key_idx][0] == '-') return -1;
    return key_idx;
}

/// 改写 `sh -c "getprop <key>"` 里的命令串。返回 true 表示已改写。
bool rewrite_shell_command(char *const argv[]) {
    const char *cmd = argv[2];
    if (!cmd) return false;

    // 找到命令行里 "getprop" 之后的那一段
    const char *p = cmd;
    while (*p) {
        if ((p == cmd || p[-1] == ' ' || p[-1] == ';' || p[-1] == '&' || p[-1] == '|') &&
            (str_starts_with(p, "getprop ") || str_starts_with(p, "getprop\t"))) {
            const char *key_start = p + 8;
            while (*key_start == ' ' || *key_start == '\t') ++key_start;
            const char *key_end = key_start;
            while (*key_end && *key_end != ' ' && *key_end != '\t' && *key_end != ';' &&
                   *key_end != '&' && *key_end != '|' && *key_end != '\n')
                ++key_end;
            if (key_end == key_start) break;

            char key[256];
            copy_to_buf(key, sizeof(key), key_start, static_cast<size_t>(key_end - key_start));
            const char *spoofed = SpoofDb::instance().get_property(key, nullptr);
            if (!spoofed) return false;

            // 拼成: <前缀>getprop <伪装值><后缀>
            char *out = g_cmd_buf;
            const size_t cap = sizeof(g_cmd_buf);
            size_t used = 0;
            auto put = [&](const char *s, size_t len) {
                if (used + len >= cap) return false;
                mem_copy(out + used, s, len);
                used += len;
                return true;
            };
            if (!put(cmd, static_cast<size_t>(key_start - cmd))) return false;
            if (!put(spoofed, str_len(spoofed))) return false;
            if (!put(key_end, str_len(key_end) + 1)) return false;

            LOGD("改写 sh -c 命令: getprop %s -> %s", key, spoofed);
            return true;
        }
        ++p;
    }
    return false;
}

bool spoof_argv(char *const argv[]) {
    if (!argv || !argv[0]) return false;

    // 1) 直接的 getprop 调用
    const int key_idx = find_getprop_key(argv);
    if (key_idx > 0 && argv[key_idx]) {
        const char *spoofed = SpoofDb::instance().get_property(argv[key_idx], nullptr);
        if (spoofed) {
            LOGD("改写子进程: %s -> %s", argv[key_idx], spoofed);
            return push_argv_replacement(argv, key_idx, const_cast<char *>(spoofed));
        }
        return false;
    }

    // 2) sh -c "getprop xxx"
    if (is_shell(argv[0]) && argv[1] && str_eq(argv[1], "-c") && argv[2]) {
        if (rewrite_shell_command(argv)) return push_argv_replacement(argv, 2, g_cmd_buf);
    }
    return false;
}

int hf_exec_common(const char *path, char *const argv[], char *const envp[],
                   int (*orig)(const char *, char *const[], char *const[])) {
    if (!orig) return -1;
    if (!options().hook_exec_getprop || !path || !argv) return orig(path, argv, envp);

    const bool changed = spoof_argv(argv);
    const int rc = orig(path, argv, envp);
    if (changed) pop_argv_replacement(argv);
    return rc;
}

int hf_execve(const char *path, char *const argv[], char *const envp[]) {
    return hf_exec_common(path, argv, envp, g_orig_execve);
}

int hf_execve2(const char *path, char *const argv[], char *const envp[]) {
    return hf_exec_common(path, argv, envp, g_orig_execve2);
}

} // namespace

void hook_install_misc() {
    if (options().spoof_uname) {
        g_orig_uname = reinterpret_cast<decltype(g_orig_uname)>(lookup_symbol("uname"));
        if (g_orig_uname) {
            plt_hook_add("uname", reinterpret_cast<void *>(&hf_uname), nullptr);
            LOGI("uname 伪装已启用: release=%s", uname_config().release);
        } else {
            LOGW("uname 未解析到, 跳过");
        }
    }

    if (options().hook_exec_getprop) {
        g_orig_execve = reinterpret_cast<decltype(g_orig_execve)>(lookup_symbol("execve"));
        if (g_orig_execve) plt_hook_add("execve", reinterpret_cast<void *>(&hf_execve), nullptr);

        g_orig_execve2 = reinterpret_cast<decltype(g_orig_execve2)>(lookup_symbol("__execve"));
        if (g_orig_execve2)
            plt_hook_add("__execve", reinterpret_cast<void *>(&hf_execve2), nullptr);

        LOGD("getprop 子进程改写: execve=%p __execve=%p",
             reinterpret_cast<void *>(g_orig_execve), reinterpret_cast<void *>(g_orig_execve2));
    }
}

} // namespace hw80
