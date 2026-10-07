// 自己实现的 PLT/GOT hook 引擎。
//
// 为什么不用 Zygisk 的 pltHookRegister:
//   Zygisk 只在 pltHookCommit() 那一刻对"已经映射的"库打补丁, 而我们的代码跑在
//   postAppSpecialize(api 即将失效)或更晚, 那时应用自己的 .so 还没加载。所以这里
//   自己实现:
//     - 解析 /proc/self/maps 找到所有已加载 ELF
//     - 解析每个 ELF 的 PT_DYNAMIC / DT_JMPREL, 把 .rela.plt 中目标符号对应的
//       GOT 槽直接改掉
//     - hook android_dlopen_ext / __loader_android_dlopen_ext / dlopen,
//       让后续加载的库也补上
#include "plt_hook.hpp"

#include <android/dlext.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include "util.hpp"

// JUMP_SLOT 重定位类型的可移植写法(用数值, 避免某些工具链缺宏)
#if defined(__aarch64__)
#define HW80_JUMP_SLOT_TYPE 1026u  // R_AARCH64_JUMP_SLOT
#elif defined(__arm__)
#define HW80_JUMP_SLOT_TYPE 22u    // R_ARM_JUMP_SLOT
#elif defined(__x86_64__)
#define HW80_JUMP_SLOT_TYPE 7u     // R_X86_64_JUMP_SLOT
#elif defined(__i386__)
#define HW80_JUMP_SLOT_TYPE 7u     // R_386_JMP_SLOT
#else
#error "unsupported architecture"
#endif

// r_info 的拆包必须和当前 ABI 的 ELF 类匹配, 不能按"结构体类型"配对:
//   ELF64: sym = r_info >> 32, type = r_info & 0xffffffff
//   ELF32: sym = r_info >> 8,  type = r_info & 0xff
// 之前按 Elf_Rela/Elf_Rel 分别硬编码 ELF64_/ELF32_ 宏, 在 arm32 上会把
// 32 位的 r_info 右移 32 位(恒为 0), 导致一个 GOT 槽都打不上补丁。
#if defined(__LP64__)
#define HW80_R_SYM(info) (static_cast<size_t>((info) >> 32))
#define HW80_R_TYPE(info) (static_cast<unsigned>((info) & 0xffffffffu))
#else
#define HW80_R_SYM(info) (static_cast<size_t>((info) >> 8))
#define HW80_R_TYPE(info) (static_cast<unsigned>((info) & 0xffu))
#endif

namespace hw80 {

namespace {

constexpr size_t kMaxHooks = 64;
constexpr size_t kMaxLibs = 1024;

struct HookEntry {
    const char *name = nullptr;  // 静态字面量, 生命周期贯穿进程
    void *replacement = nullptr;
    void *original = nullptr;
};

struct LibKey {
    dev_t dev = 0;
    ino_t inode = 0;
};

// 构建期单线程写入, 之后只读。
HookEntry g_hooks[kMaxHooks]{};
size_t g_hook_count = 0;

// seen 列表可能被多线程的 dlopen 并发访问, 用自旋锁保护。
LibKey g_seen_libs[kMaxLibs]{};
size_t g_seen_count = 0;
int g_seen_lock = 0;

bool g_dlopen_hooked = false;

// 同一线程内防止打补丁过程中递归触发扫描。
thread_local int g_scan_depth = 0;

struct ScanGuard {
    const bool active;
    ScanGuard() : active(g_scan_depth == 0) {
        if (active) g_scan_depth = 1;
    }
    ~ScanGuard() {
        if (active) g_scan_depth = 0;
    }
    explicit operator bool() const { return active; }
};

struct SpinLock {
    bool held = false;
    explicit SpinLock(int &lock) : lock_(lock) {
        while (__atomic_exchange_n(&lock_, 1, __ATOMIC_ACQUIRE) != 0) {
            // 自旋; 持锁区间非常短(只有几百次比较)
        }
        held = true;
    }
    ~SpinLock() {
        if (held) __atomic_store_n(&lock_, 0, __ATOMIC_RELEASE);
    }
    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;

private:
    int &lock_;
};

bool lib_seen_locked(dev_t dev, ino_t inode) {
    for (size_t i = 0; i < g_seen_count; ++i)
        if (g_seen_libs[i].dev == dev && g_seen_libs[i].inode == inode) return true;
    return false;
}

void lib_mark_seen_locked(dev_t dev, ino_t inode) {
    if (g_seen_count < kMaxLibs) {
        g_seen_libs[g_seen_count].dev = dev;
        g_seen_libs[g_seen_count].inode = inode;
        ++g_seen_count;
    }
}

int find_hook(const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < g_hook_count; ++i)
        if (str_eq(g_hooks[i].name, name)) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------
// ELF 类型
// ---------------------------------------------------------------------------
#if defined(__LP64__)
using Elf_Ehdr = Elf64_Ehdr;
using Elf_Phdr = Elf64_Phdr;
using Elf_Dyn = Elf64_Dyn;
using Elf_Sym = Elf64_Sym;
using Elf_Rela = Elf64_Rela;
using Elf_Rel = Elf64_Rel;
using SlotT = uint64_t;
#else
using Elf_Ehdr = Elf32_Ehdr;
using Elf_Phdr = Elf32_Phdr;
using Elf_Dyn = Elf32_Dyn;
using Elf_Sym = Elf32_Sym;
using Elf_Rela = Elf32_Rela;
using Elf_Rel = Elf32_Rel;
using SlotT = uint32_t;
#endif

inline size_t reloc_sym(const Elf_Rela &r) { return HW80_R_SYM(r.r_info); }
inline size_t reloc_sym(const Elf_Rel &r) { return HW80_R_SYM(r.r_info); }

/// 覆盖一个 GOT 槽。返回 true 表示本次真的改动了。
bool patch_slot(uintptr_t addr, SlotT value) {
    if (addr == 0) return false;
    auto *slot = reinterpret_cast<SlotT *>(addr);
    if (__atomic_load_n(slot, __ATOMIC_RELAXED) == value) return false;
    __atomic_store_n(slot, value, __ATOMIC_RELAXED);
    return true;
}

// ---------------------------------------------------------------------------
// 对一个 ELF 打补丁
// ---------------------------------------------------------------------------
size_t patch_elf(uintptr_t base, uintptr_t map_end, bool *is_elf) {
    *is_elf = false;
    const auto *ehdr = reinterpret_cast<const Elf_Ehdr *>(base);
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return 0;
    if (ehdr->e_ident[EI_CLASS] !=
        (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32))
        return 0;
    *is_elf = true;

    // 只处理共享库 / PIE 可执行文件
    if (ehdr->e_type != ET_DYN && ehdr->e_type != ET_EXEC) return 0;
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return 0;
    if (ehdr->e_phentsize != sizeof(Elf_Phdr)) return 0;

    // 程序头表必须落在本段映射范围内, 否则下面的遍历会读到别的映射
    const uintptr_t ph_end = base + ehdr->e_phoff +
                             static_cast<uintptr_t>(ehdr->e_phnum) * sizeof(Elf_Phdr);
    if (ehdr->e_phoff > map_end - base || ph_end > map_end) return 0;

    const auto *phdr = reinterpret_cast<const Elf_Phdr *>(base + ehdr->e_phoff);

    // 算一遍 ELF 的 load bias:
    //   映射基址 = base + (p_vaddr - p_offset)
    //   bias     = 映射基址 - p_vaddr = base - p_offset
    // 共享库第一个 PT_LOAD 的 p_offset 恒为 0, 于是 bias == base。
    // 若 p_offset 非 0(理论上可能是恶意/畸形 ELF), 这个假设不成立, 直接放弃。
    const Elf_Phdr *dyn_ph = nullptr;
    uintptr_t bias = base;
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type != PT_LOAD) continue;
        if (phdr[i].p_offset != 0) {
            LOGD("跳过 %p: 首个 PT_LOAD 的 p_offset=%lu != 0", reinterpret_cast<void *>(base),
                 static_cast<unsigned long>(phdr[i].p_offset));
            return 0;
        }
        bias = base - phdr[i].p_offset;
        break;  // 第一个 PT_LOAD
    }
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn_ph = &phdr[i];
            break;
        }
    }
    if (!dyn_ph) return 0;

    const auto *dyn = reinterpret_cast<const Elf_Dyn *>(bias + dyn_ph->p_vaddr);
    const size_t dyn_count = dyn_ph->p_memsz / sizeof(Elf_Dyn);

    uintptr_t jmprel = 0, symtab = 0, strtab = 0, rela = 0, rel = 0;
    size_t pltrelsz = 0, relasz = 0, relsz = 0;
    uintptr_t pltrel = 0;  // DT_PLTREL: DT_RELA 或 DT_REL

    for (size_t i = 0; i < dyn_count; ++i) {
        if (dyn[i].d_tag == DT_NULL) break;
        switch (dyn[i].d_tag) {
            case DT_SYMTAB: symtab = static_cast<uintptr_t>(dyn[i].d_un.d_ptr); break;
            case DT_STRTAB: strtab = static_cast<uintptr_t>(dyn[i].d_un.d_ptr); break;
            case DT_JMPREL: jmprel = static_cast<uintptr_t>(dyn[i].d_un.d_ptr); break;
            case DT_PLTRELSZ: pltrelsz = static_cast<size_t>(dyn[i].d_un.d_val); break;
            case DT_PLTREL: pltrel = static_cast<uintptr_t>(dyn[i].d_un.d_val); break;
            case DT_RELA: rela = static_cast<uintptr_t>(dyn[i].d_un.d_ptr); break;
            case DT_RELASZ: relasz = static_cast<size_t>(dyn[i].d_un.d_val); break;
            case DT_REL: rel = static_cast<uintptr_t>(dyn[i].d_un.d_ptr); break;
            case DT_RELSZ: relsz = static_cast<size_t>(dyn[i].d_un.d_val); break;
            default: break;
        }
    }

    // DT_* 里 d_ptr 在内存中已被动态链接器重定位成绝对地址, 直接可用。
    if (!symtab || !strtab) return 0;
    const auto *sym = reinterpret_cast<const Elf_Sym *>(symtab);
    const char *str = reinterpret_cast<const char *>(strtab);
    if (!sym || !str) return 0;

    // GOT 槽的可写区间: 用整个 ELF 的映射范围做上界检查, 避免畸形 ELF
    // 导致越界改内存。
    const uintptr_t limit_lo = base;
    const uintptr_t limit_hi = map_end;

    auto in_range = [&](uintptr_t target) {
        return target >= limit_lo && target + sizeof(SlotT) <= limit_hi;
    };

    // 把目标符号的 GOT 槽改成替换实现
    auto handle_rela = [&](const Elf_Rela *r, size_t n) -> size_t {
        size_t patched = 0;
        for (size_t i = 0; i < n; ++i) {
            if (HW80_R_TYPE(r[i].r_info) != HW80_JUMP_SLOT_TYPE) continue;
            const int h = find_hook(str + sym[reloc_sym(r[i])].st_name);
            if (h < 0) continue;
            if (!in_range(r[i].r_offset)) continue;
            if (patch_slot(r[i].r_offset, reinterpret_cast<SlotT>(g_hooks[h].replacement)))
                ++patched;
        }
        return patched;
    };
    auto handle_rel = [&](const Elf_Rel *r, size_t n) -> size_t {
        size_t patched = 0;
        for (size_t i = 0; i < n; ++i) {
            if (HW80_R_TYPE(r[i].r_info) != HW80_JUMP_SLOT_TYPE) continue;
            const int h = find_hook(str + sym[reloc_sym(r[i])].st_name);
            if (h < 0) continue;
            if (!in_range(r[i].r_offset)) continue;
            if (patch_slot(r[i].r_offset, reinterpret_cast<SlotT>(g_hooks[h].replacement)))
                ++patched;
        }
        return patched;
    };

    size_t patched = 0;

    // JMPREL 表: 用 DT_PLTREL 判断条目是 RELA 还是 REL。
    // 不能按 `pltrelsz % sizeof(...)` 猜 —— 同一 ABI 下 sizeof(Rela) 与
    // sizeof(Rel) 相等(arm32 都是 8, arm64 都是 24)... 实际上 arm32 的
    // Elf32_Rela 是 12 字节, 于是"能被 24 整除"的表会被误判成 RELA 而静默跳过。
    if (jmprel && pltrelsz) {
        const bool is_rela = (pltrel == DT_RELA);
        if (is_rela && pltrelsz % sizeof(Elf_Rela) == 0) {
            patched += handle_rela(reinterpret_cast<const Elf_Rela *>(jmprel),
                                   pltrelsz / sizeof(Elf_Rela));
        } else if (!is_rela && pltrelsz % sizeof(Elf_Rel) == 0) {
            patched += handle_rel(reinterpret_cast<const Elf_Rel *>(jmprel),
                                  pltrelsz / sizeof(Elf_Rel));
        } else if (pltrelsz % sizeof(Elf_Rela) == 0) {
            // DT_PLTREL 缺失或异常时退回按大小兜底
            patched += handle_rela(reinterpret_cast<const Elf_Rela *>(jmprel),
                                   pltrelsz / sizeof(Elf_Rela));
        } else if (pltrelsz % sizeof(Elf_Rel) == 0) {
            patched += handle_rel(reinterpret_cast<const Elf_Rel *>(jmprel),
                                  pltrelsz / sizeof(Elf_Rel));
        }
    }

    // 少数库把 JUMP_SLOT 放在 DT_RELA 里
    if (rela && relasz) {
        patched += handle_rela(reinterpret_cast<const Elf_Rela *>(rela),
                               relasz / sizeof(Elf_Rela));
    }
    if (rel && relsz) {
        patched += handle_rel(reinterpret_cast<const Elf_Rel *>(rel), relsz / sizeof(Elf_Rel));
    }

    if (patched) {
        __builtin___clear_cache(reinterpret_cast<char *>(base),
                                reinterpret_cast<char *>(base + 0x1000));
    }
    return patched;
}

// ---------------------------------------------------------------------------
// maps 扫描
// ---------------------------------------------------------------------------
struct ScanState {
    bool new_only = false;
    size_t patched = 0;
    size_t libs = 0;
};

struct LibCollector : MapVisitor {
    ScanState *state = nullptr;
    dev_t cur_dev = 0;
    ino_t cur_inode = 0;
    bool have_cur = false;

    bool visit(const MapEntry &e) override {
        if (!e.path || e.path[0] != '/') return true;  // 跳过 [vdso] / [anon:*]

        const bool same_file = have_cur && e.dev == cur_dev && e.inode == cur_inode;
        if (same_file) return true;

        cur_dev = e.dev;
        cur_inode = e.inode;
        have_cur = true;

        // 查表 + 登记必须在同一个临界区里完成, 否则两个并发的 dlopen
        // 可能同时判定"没见过"而重复打补丁(现在是幂等的, 但计数会错)。
        bool already = false;
        {
            SpinLock lock(g_seen_lock);
            already = lib_seen_locked(e.dev, e.inode);
            if (state->new_only && already) return true;
            if (!already) lib_mark_seen_locked(e.dev, e.inode);
        }

        bool is_elf = false;
        const size_t n = patch_elf(e.start, e.end, &is_elf);
        if (!is_elf) return true;  // 不是 ELF(例如 data 文件映射)

        ++state->libs;
        state->patched += n;
        return true;
    }
};

size_t scan(bool new_only) {
    ScanGuard guard;
    if (!guard) return 0;

    ScanState st{};
    st.new_only = new_only;
    LibCollector collector{};
    collector.state = &st;
    foreach_map(collector);
    return st.patched;
}

// ---------------------------------------------------------------------------
// dlopen 系列 hook: 新库加载后补打补丁
// ---------------------------------------------------------------------------
using DlopenExtFn = void *(*)(const char *filename, int flags, const android_dlextinfo *info);
using DlopenFn = void *(*)(const char *filename, int flags);

DlopenExtFn g_orig_android_dlopen_ext = nullptr;
DlopenExtFn g_orig_loader_dlopen_ext = nullptr;
DlopenFn g_orig_dlopen = nullptr;

/// 调用原始 dlopen 并重新扫描。
///
/// 关键点: 真正的 dlopen 会在这段调用内部跑新库的构造函数, 而构造函数里
/// 往往还会再 dlopen 别的库。如果此时 g_scan_depth 已经置位(即我们正处在
/// 自己的扫描帧里), 嵌套的 scan() 会因为 ScanGuard 不生效而直接返回 0,
/// 于是"被构造函数 dlopen 出来的库"永远补不上补丁。
/// 所以这里先把扫描标记清掉, 让嵌套 dlopen 各自完成自己的扫描, 最后再
/// 统一补一次(幂等, new_only 会跳过已登记的库)。
void *hf_android_dlopen_ext(const char *filename, int flags, const android_dlextinfo *info) {
    if (!g_orig_android_dlopen_ext) return nullptr;
    const int saved = g_scan_depth;
    g_scan_depth = 0;
    void *handle = g_orig_android_dlopen_ext(filename, flags, info);
    g_scan_depth = saved;
    if (filename && scan(/*new_only=*/true)) {
        LOGD("新库 %s 加载完成, 已补充 PLT 补丁", filename);
    }
    return handle;
}

void *hf_loader_dlopen_ext(const char *filename, int flags, const android_dlextinfo *info) {
    if (!g_orig_loader_dlopen_ext) return nullptr;
    const int saved = g_scan_depth;
    g_scan_depth = 0;
    void *handle = g_orig_loader_dlopen_ext(filename, flags, info);
    g_scan_depth = saved;
    if (filename) scan(/*new_only=*/true);
    return handle;
}

void *hf_dlopen(const char *filename, int flags) {
    if (!g_orig_dlopen) return nullptr;
    const int saved = g_scan_depth;
    g_scan_depth = 0;
    void *handle = g_orig_dlopen(filename, flags);
    g_scan_depth = saved;
    if (filename) scan(/*new_only=*/true);
    return handle;
}

void install_dlopen_hooks() {
    // 先解析原始实现(dlsym 拿到的是真实实现), 再注册替换。
    if (void *p = lookup_symbol("android_dlopen_ext")) {
        g_orig_android_dlopen_ext = reinterpret_cast<DlopenExtFn>(p);
        if (plt_hook_add("android_dlopen_ext",
                         reinterpret_cast<void *>(&hf_android_dlopen_ext), nullptr))
            g_dlopen_hooked = true;
    }
    if (void *p = lookup_symbol("__loader_android_dlopen_ext")) {
        g_orig_loader_dlopen_ext = reinterpret_cast<DlopenExtFn>(p);
        if (plt_hook_add("__loader_android_dlopen_ext",
                         reinterpret_cast<void *>(&hf_loader_dlopen_ext), nullptr))
            g_dlopen_hooked = true;
    }
    if (void *p = lookup_symbol("dlopen")) {
        g_orig_dlopen = reinterpret_cast<DlopenFn>(p);
        if (plt_hook_add("dlopen", reinterpret_cast<void *>(&hf_dlopen), nullptr))
            g_dlopen_hooked = true;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
void plt_hook_reset() {
    g_hook_count = 0;
    {
        SpinLock lock(g_seen_lock);
        g_seen_count = 0;
    }
    g_dlopen_hooked = false;
}

bool plt_hook_add(const char *symbol, void *replacement, void **old_out) {
    if (!symbol || !replacement) return false;
    if (g_hook_count >= kMaxHooks) {
        LOGE("hook 表已满(%zu), 丢弃 %s", kMaxHooks, symbol);
        return false;
    }
    HookEntry &e = g_hooks[g_hook_count];
    e.name = symbol;  // 静态字面量, 无需复制
    e.replacement = replacement;
    e.original = lookup_symbol(symbol);
    if (old_out) *old_out = e.original;
    if (!e.original)
        LOGW("符号 %s 没解析到原始实现, hook 内不能调用它", symbol);
    ++g_hook_count;
    return true;
}

size_t plt_hook_apply() {
    if (g_hook_count == 0) return 0;
    install_dlopen_hooks();
    {
        SpinLock lock(g_seen_lock);
        g_seen_count = 0;  // 全量扫描
    }
    const size_t patched = scan(/*new_only=*/false);
    LOGI("PLT 补丁: %zu 个 GOT 槽已替换, 规则 %zu 条, dlopen hook %s", patched, g_hook_count,
         g_dlopen_hooked ? "已装" : "未装");
    return patched;
}

size_t plt_hook_apply_new() { return scan(/*new_only=*/true); }

bool plt_hook_dlopen_hooked() { return g_dlopen_hooked; }

} // namespace hw80
