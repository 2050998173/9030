// 系统属性拦截。
//
// 覆盖四层调用路径:
//   1. __system_property_get           —— 绝大多数 native 代码走这里
//   2. __system_property_read          —— 老 API, 老库还在用
//   3. __system_property_read_callback —— API 26+ 新 API
//   4. __system_property_find          —— 拿句柄缓存复用(实验性, 默认关)
//
// Java 层 (android.os.SystemProperties.get / Build.*) 由 hook_jni.cpp 处理。
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "config.hpp"
#include "hooks.hpp"
#include "plt_hook.hpp"
#include "spoof_db.hpp"
#include "util.hpp"

namespace hw80 {
namespace {

// --- 原始实现 ---
int (*g_orig_get)(const char *name, char *value) = nullptr;
int (*g_orig_read)(const void *pi, char *name, char *value) = nullptr;
void (*g_orig_read_cb)(const void *pi, void (*cb)(void *, const char *, const char *, uint32_t),
                       void *cookie) = nullptr;
const void *(*g_orig_find)(const char *name) = nullptr;

using PropCallback = void (*)(void *, const char *, const char *, uint32_t);

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
bool lookup_override(const char *name, const char **out, size_t *out_len) {
    if (!name || !*name) return false;
    const char *v = SpoofDb::instance().get_property(name, out_len);
    if (!v) return false;
    *out = v;
    return true;
}

/// 通过原始 read_callback 把真实 prop_info 读成字符串。
bool read_real(const void *pi, char *name_out, size_t name_cap, char *value_out,
               size_t value_cap);

struct CaptureCtx {
    char *name = nullptr;
    size_t name_cap = 0;
    char *value = nullptr;
    size_t value_cap = 0;
    bool ok = false;
};

void capture_callback(void *cookie, const char *name, const char *value, uint32_t serial) {
    (void)serial;
    auto *c = static_cast<CaptureCtx *>(cookie);
    if (!c) return;
    if (c->name) copy_to_buf(c->name, c->name_cap, name);
    if (c->value) copy_to_buf(c->value, c->value_cap, value);
    c->ok = true;
}

bool read_real(const void *pi, char *name_out, size_t name_cap, char *value_out,
               size_t value_cap) {
    if (!pi) return false;
    if (g_orig_read_cb) {
        CaptureCtx c{name_out, name_cap, value_out, value_cap, false};
        g_orig_read_cb(pi, capture_callback, &c);
        return c.ok;
    }
    if (g_orig_read) {
        // 没有 read_callback 时退到 __system_property_read:
        // 它的 name/value 参数必须至少是 32 / 92 字节, 用临时缓冲再按 cap 截断。
        char nm[32] = {};
        char val[92] = {};
        if (g_orig_read(pi, nm, val) < 0) return false;
        if (name_out) copy_to_buf(name_out, name_cap, nm);
        if (value_out) copy_to_buf(value_out, value_cap, val);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 伪造 prop_info (供 __system_property_find 使用)
// ---------------------------------------------------------------------------
// bionic 的 prop_info 内存布局(serial 作为普通 uint32 的经典布局):
//   uint32_t serial;    // 位 0 = dirty; 高 8 位在部分版本里用于编码长度
//   char value[92];     // legacy 取值格式
//   char name[32];
// 我们不直接按 struct 访问(那要依赖内部头文件), 而是自己按 128 字节一条
// 摆好, 只要 serial / value / name 三者的偏移对就行。
//
// serial 的取值很关键: bionic 用 serial 的高 8 位表示值的长度。
// 如果只写个 2, 长度就是 0, 于是 SystemProperties.Handle.get() 之类的
// 调用方会读到空字符串 —— 比不 hook 还糟。所以必须写成
// `(len << 24) | 2`。位 1 置 1 是为了保证位 0(部分版本用作 dirty 标志)为 0。
constexpr size_t kFakePropValueMax = 91;  // 必须留一个 '\0'
constexpr size_t kFakePropRecord = 128;
constexpr size_t kMaxFakeProps = 256;
constexpr size_t kFakePropsBytes = kFakePropRecord * kMaxFakeProps;
constexpr uint32_t kFakePropSerialFlags = 2u;  // 位 0 必须为 0

alignas(16) unsigned char g_fake_props[kFakePropsBytes];
// 已发布的记录数。构建期单线程写完后用 release 发布, 运行期只读。
uint32_t g_fake_prop_count = 0;
constexpr uint32_t kFakePropsMagic = 0x48573830u;  // "HW80"
uint32_t g_fake_props_magic = 0;

/// 判断指针是否落在我们伪造的 prop_info 区里。
bool in_fake_range(const void *p, const char **out_name, const char **out_value) {
    if (!p || g_fake_props_magic != kFakePropsMagic) return false;
    const uint32_t count = __atomic_load_n(&g_fake_prop_count, __ATOMIC_ACQUIRE);
    if (count == 0) return false;

    const auto *base = g_fake_props;
    const auto *q = static_cast<const unsigned char *>(p);
    if (q < base || q >= base + count * kFakePropRecord) return false;
    if ((static_cast<size_t>(q - base) % kFakePropRecord) != 0) return false;

    const char *rec_value = reinterpret_cast<const char *>(q + sizeof(uint32_t));
    const char *rec_name = rec_value + 92;
    if (out_name) *out_name = rec_name;
    if (out_value) *out_value = rec_value;
    return true;
}

/// 在【单线程】的安装阶段调用一次, 把所有属性预先做成伪造 prop_info。
/// 这样运行期 __system_property_find 只需要只读查表, 不会出现
/// "两个线程同时分配同一条记录" 的竞争。
void build_fake_props() {
    if (g_fake_props_magic == kFakePropsMagic) return;
    mem_set(g_fake_props, 0, sizeof(g_fake_props));

    // 确保这一页可写(正常情况下本来就可写)
    const uintptr_t page = reinterpret_cast<uintptr_t>(g_fake_props) & ~(uintptr_t)0xfff;
    const size_t span = reinterpret_cast<uintptr_t>(g_fake_props) + sizeof(g_fake_props) - page;
    if (mprotect(reinterpret_cast<void *>(page), span, PROT_READ | PROT_WRITE) != 0) {
        LOGW("mprotect 伪造 prop_info 区失败, __system_property_find 伪造不可用");
        return;
    }

    SpoofDb &db = SpoofDb::instance();
    uint32_t n = 0;
    const size_t total = db.property_count();
    for (size_t i = 0; i < total && n < kMaxFakeProps; ++i) {
        const char *name = nullptr;
        const char *value = nullptr;
        size_t vlen = 0;
        if (!db.property_at(i, &name, &value, &vlen)) continue;
        if (!name || !value) continue;
        if (vlen > kFakePropValueMax) continue;      // 太长的只能靠 get/read 拦截
        if (str_len(name) + 1 > 32) continue;        // prop_info 的 name 只有 32 字节

        unsigned char *rec = g_fake_props + n * kFakePropRecord;
        auto *serial = reinterpret_cast<uint32_t *>(rec);
        char *rec_value = reinterpret_cast<char *>(rec + sizeof(uint32_t));
        char *rec_name = rec_value + 92;

        if (vlen) mem_copy(rec_value, value, vlen);
        rec_value[vlen] = '\0';
        const size_t nlen = str_len(name);
        mem_copy(rec_name, name, nlen);
        rec_name[nlen] = '\0';

        // 高 8 位 = 长度, 位 0 = 0 (非 dirty)
        __atomic_store_n(serial,
                         (static_cast<uint32_t>(vlen) << 24) | kFakePropSerialFlags,
                         __ATOMIC_RELAXED);
        ++n;
    }

    __atomic_store_n(&g_fake_prop_count, n, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fake_props_magic, kFakePropsMagic, __ATOMIC_RELEASE);
    LOGI("__system_property_find 伪造表: %u 条 (共 %zu 条属性)", n, total);
}

/// 运行期只读查表。
const void *fake_prop_for(const char *name) {
    const uint32_t count = __atomic_load_n(&g_fake_prop_count, __ATOMIC_ACQUIRE);
    if (count == 0) return nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *rec = g_fake_props + i * kFakePropRecord;
        const char *rec_name = reinterpret_cast<const char *>(rec + sizeof(uint32_t) + 92);
        if (str_eq(rec_name, name)) return rec;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// hook 实现
// ---------------------------------------------------------------------------
int hf_system_property_get(const char *name, char *value) {
    const char *v = nullptr;
    size_t len = 0;
    if (lookup_override(name, &v, &len)) {
        if (!value) return static_cast<int>(len);
        mem_copy(value, v, len);
        value[len] = '\0';
        LOGD("__system_property_get(%s) -> %s", name, v);
        return static_cast<int>(len);
    }
    if (g_orig_get) return g_orig_get(name, value);
    if (value) value[0] = '\0';
    return 0;
}

int hf_system_property_read(const void *pi, char *name, char *value) {
    // bionic 的约定: name 指向 PROP_NAME_MAX(32) 字节, value 指向 PROP_VALUE_MAX(92)
    char fname_buf[32];
    char fvalue_buf[92];
    const char *fname = nullptr;
    const char *fvalue = nullptr;

    if (in_fake_range(pi, &fname, &fvalue)) {
        const size_t vlen = str_len(fvalue);
        const size_t nlen = str_len(fname);
        if (name) {
            mem_copy(name, fname, nlen);
            name[nlen] = '\0';
        }
        if (value) {
            mem_copy(value, fvalue, vlen);
            value[vlen] = '\0';
        }
        // bionic 返回的是长度 + 1
        return static_cast<int>(vlen + 1);
    }

    if (!pi) return -1;
    if (!read_real(pi, fname_buf, sizeof(fname_buf), fvalue_buf, sizeof(fvalue_buf))) return -1;

    const char *v = nullptr;
    size_t len = 0;
    const char *out_value = fvalue_buf;
    if (fname_buf[0] && lookup_override(fname_buf, &v, &len)) {
        out_value = v;
    }
    if (name) copy_to_buf(name, 32, fname_buf);
    if (value) copy_to_buf(value, 92, out_value);
    return static_cast<int>(str_len(out_value) + 1);
}

// 给原始 read_callback 用的转发上下文
struct ForwardCtx {
    PropCallback cb = nullptr;
    void *cookie = nullptr;
};

void forward_callback(void *raw, const char *name, const char *value, uint32_t serial) {
    auto *f = static_cast<ForwardCtx *>(raw);
    if (!f || !f->cb) return;
    const char *ov = nullptr;
    size_t len = 0;
    if (lookup_override(name, &ov, &len)) {
        LOGD("read_callback(%s) -> %s", name, ov);
        // 保持长度编码与值一致, 否则调用方按 serial 取长度会读到截断值
        const uint32_t new_serial =
            (static_cast<uint32_t>(len) << 24) | (serial & 0x00ffffffu);
        f->cb(f->cookie, name, ov, new_serial);
    } else {
        f->cb(f->cookie, name, value, serial);
    }
}

void hf_system_property_read_callback(const void *pi, PropCallback cb, void *cookie) {
    if (!cb) return;

    const char *fname = nullptr;
    const char *fvalue = nullptr;
    if (in_fake_range(pi, &fname, &fvalue)) {
        const uint32_t serial = *reinterpret_cast<const volatile uint32_t *>(pi);
        cb(cookie, fname, fvalue, serial);
        return;
    }

    if (!g_orig_read_cb) {
        cb(cookie, "", "", 0);
        return;
    }
    ForwardCtx f{cb, cookie};
    g_orig_read_cb(pi, forward_callback, &f);
}

const void *hf_system_property_find(const char *name) {
    if (!name) return nullptr;
    // 运行期只读查表, 不做任何写入
    if (const void *fake = fake_prop_for(name)) return fake;
    return g_orig_find ? g_orig_find(name) : nullptr;
}

} // namespace

void hook_resolve_originals() {
    g_orig_get = reinterpret_cast<decltype(g_orig_get)>(lookup_symbol("__system_property_get"));
    g_orig_read = reinterpret_cast<decltype(g_orig_read)>(lookup_symbol("__system_property_read"));
    g_orig_read_cb =
        reinterpret_cast<decltype(g_orig_read_cb)>(lookup_symbol("__system_property_read_callback"));
    g_orig_find = reinterpret_cast<decltype(g_orig_find)>(lookup_symbol("__system_property_find"));

    LOGD("原始符号: get=%p read=%p read_cb=%p find=%p",
         reinterpret_cast<void *>(g_orig_get), reinterpret_cast<void *>(g_orig_read),
         reinterpret_cast<void *>(g_orig_read_cb), reinterpret_cast<void *>(g_orig_find));

    // 文件覆盖那部分在另一个编译单元里
    file_overlay_resolve_originals();
}

void hook_install_property() {
    if (!g_orig_get) LOGW("__system_property_get 未解析到, 属性拦截可能不生效");

    plt_hook_add("__system_property_get", reinterpret_cast<void *>(&hf_system_property_get),
                 nullptr);
    if (g_orig_read)
        plt_hook_add("__system_property_read", reinterpret_cast<void *>(&hf_system_property_read),
                     nullptr);
    if (g_orig_read_cb)
        plt_hook_add("__system_property_read_callback",
                     reinterpret_cast<void *>(&hf_system_property_read_callback), nullptr);
}

void hook_install_property_find() {
    if (!g_orig_find) {
        LOGW("__system_property_find 未解析到, 跳过句柄伪造");
        return;
    }
    build_fake_props();
    plt_hook_add("__system_property_find", reinterpret_cast<void *>(&hf_system_property_find),
                 nullptr);
    LOGW("已启用实验性 __system_property_find 伪造 (值超过 91 字节的属性不会被拦截)");
}

} // namespace hw80
