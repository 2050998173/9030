#include "spoof_db.hpp"

#include "util.hpp"

namespace hw80 {

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------
char *Arena::alloc(size_t bytes) {
    // 8 字节对齐
    const size_t aligned = (bytes + 7u) & ~static_cast<size_t>(7u);
    if (aligned == 0 || used_ + aligned > kCapacity) return nullptr;
    char *p = data_ + used_;
    used_ += static_cast<uint32_t>(aligned);
    return p;
}

const char *Arena::dup(const char *s, size_t len) {
    if (!s) return nullptr;
    char *p = alloc(len + 1);
    if (!p) return nullptr;
    if (len) mem_copy(p, s, len);
    p[len] = '\0';
    return p;
}

// ---------------------------------------------------------------------------
// SpoofDb
// ---------------------------------------------------------------------------
SpoofDb &SpoofDb::instance() {
    // 函数内 static + POD 成员 => 常量初始化, 无 .init_array 依赖。
    static SpoofDb db;
    return db;
}

bool SpoofDb::add_property(const char *name, const char *value, size_t value_len) {
    if (!name || !*name) return false;
    if (prop_count_ >= kMaxProps) return false;

    const size_t name_len = str_len(name);
    const char *name_copy = arena_.dup(name, name_len);
    if (!name_copy) return false;

    const char *value_copy = arena_.dup(value ? value : "", value ? value_len : 0);
    if (!value_copy) return false;

    prop_names_[prop_count_] = name_copy;
    prop_values_[prop_count_] = value_copy;
    prop_lens_[prop_count_] = static_cast<uint32_t>(value ? value_len : 0);
    ++prop_count_;
    return true;
}

bool SpoofDb::add_text(const char *path, const char *suffix, const char *data, size_t size) {
    if (!path || !data) return false;
    if (text_count_ >= kMaxTexts) return false;

    const char *path_copy = arena_.dup(path, str_len(path));
    if (!path_copy) return false;

    const char *suffix_copy = suffix ? arena_.dup(suffix, str_len(suffix)) : nullptr;
    if (suffix && !suffix_copy) return false;

    char *data_copy = arena_.alloc(size + 1);
    if (!data_copy) return false;
    if (size) mem_copy(data_copy, data, size);
    data_copy[size] = '\0';

    TextEntry &e = text_[text_count_];
    e.path = path_copy;
    e.suffix = suffix_copy;
    e.data = data_copy;
    e.size = size;
    ++text_count_;
    return true;
}

bool SpoofDb::add_app(const char *name) {
    if (!name || !*name) return false;
    if (app_count_ >= kMaxApps) return false;

    const char *copy = arena_.dup(name, str_len(name));
    if (!copy) return false;
    app_names_[app_count_++] = copy;
    return true;
}

const char *SpoofDb::get_property(const char *name, size_t *out_len) const {
    if (!name) return nullptr;
    for (uint32_t i = 0; i < prop_count_; ++i) {
        if (str_eq(prop_names_[i], name)) {
            if (out_len) *out_len = prop_lens_[i];
            return prop_values_[i];
        }
    }
    return nullptr;
}

bool SpoofDb::property_at(size_t index, const char **name, const char **value,
                          size_t *value_len) const {
    if (index >= prop_count_) return false;
    if (name) *name = prop_names_[index];
    if (value) *value = prop_values_[index];
    if (value_len) *value_len = prop_lens_[index];
    return true;
}

bool SpoofDb::app_matches(const char *process_name) const {
    if (!process_name || !*process_name) return false;
    if (app_count_ == 0) return false;

    const size_t pn_len = str_len(process_name);
    for (uint32_t i = 0; i < app_count_; ++i) {
        const char *entry = app_names_[i];
        if (str_eq(entry, process_name)) return true;

        // 条目写成 "包名:进程名" 时必须完全一致(上面已比过), 不再做前缀匹配。
        if (str_contains(entry, ":")) continue;

        // 条目是纯包名时, "包名:任意子进程" 也算命中。
        const size_t el = str_len(entry);
        if (el < pn_len && process_name[el] == ':') {
            bool prefix_ok = true;
            for (size_t k = 0; k < el; ++k) {
                if (process_name[k] != entry[k]) { prefix_ok = false; break; }
            }
            if (prefix_ok) return true;
        }
    }
    return false;
}

} // namespace hw80
