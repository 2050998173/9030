// Zygisk 模块入口。
//
// 执行流程:
//   onLoad()
//     注册模块, 保存 Api 和 JNIEnv
//
//   preAppSpecialize()
//     1. 取进程名(包名), 不在伪装名单里就直接返回 —— Zygisk 会卸载模块,
//        该进程完全不受影响
//     2. 通过 companion( root 进程 )读回 /data/adb/hw80pro/hw80pro.conf
//     3. 解析配置, 构建伪装数据库
//     4. 解析原始函数指针 -> 注册 hook 规则 -> 打 PLT 补丁
//        这一步必须在这里做完: postAppSpecialize 之后 Zygisk 的 Api 会失效
//
//   postAppSpecialize()
//     5. 替换 android.os.SystemProperties 的 native 方法, 这一步要求
//        ClassLoader 已经可用, 所以放在 specialize 之后
#include <jni.h>
#include <fcntl.h>
#include <unistd.h>

#include "companion.hpp"
#include "config.hpp"
#include "hooks.hpp"
#include "plt_hook.hpp"
#include "profile_data.hpp"
#include "spoof_db.hpp"
#include "util.hpp"
#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

namespace {

constexpr size_t kMaxConfBytes = 256 * 1024;
constexpr size_t kMaxCpuInfoBytes = 64 * 1024;

// 配置缓冲区放在静态存储而不是栈上: 解析期间 SpoofDb 会持有指向它的
// string_view, 虽然实际数据都会被复制进 arena, 但保持生命周期清晰更安全。
char g_conf_buf[kMaxConfBytes];
char g_cpuinfo_buf[kMaxCpuInfoBytes];

bool jstring_to_utf8(JNIEnv *env, jstring s, char *out, size_t cap) {
    out[0] = '\0';
    if (!env || !s || cap == 0) return false;
    const char *utf = env->GetStringUTFChars(s, nullptr);
    if (!utf) return false;
    copy_to_buf(out, cap, utf);
    env->ReleaseStringUTFChars(s, utf);
    return true;
}

class Mate80ProModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        // 注意: 这里【不能】用 app_matches 做过滤。
        // 此刻 SpoofDb 还是空的(app_count_ == 0, app_matches 恒返回 false),
        // 真正的名单来自配置, 必须等 config_parse() 之后再判断(见下面第二步)。
        char process_name[512];
        if (!jstring_to_utf8(env_, args->nice_name, process_name, sizeof(process_name))) {
            return;
        }

        log_init();
        log_set_debug(false);

        // ---- 1. 取配置 ----
        size_t conf_len = 0;
        size_t cpuinfo_len = 0;
        bool got = false;

        const int sock = api_->connectCompanion();
        if (sock >= 0) {
            got = companion_fetch(sock, g_conf_buf, sizeof(g_conf_buf), &conf_len,
                                  g_cpuinfo_buf, sizeof(g_cpuinfo_buf), &cpuinfo_len);
            // 用全局作用域避免将来误调用被 hook 的 libc 包装
            ::close(sock);
            if (!got) LOGW("从 companion 读取配置失败, 使用内置默认配置");
        } else {
            LOGW("连接 companion 失败, 使用内置默认配置");
        }

        if (!got || conf_len == 0) {
            copy_to_buf(g_conf_buf, sizeof(g_conf_buf), kDefaultConf.data(), kDefaultConf.size());
            conf_len = str_len(g_conf_buf);
            cpuinfo_len = 0;  // 用编译内置的 cpuinfo 模板
        }

        // ---- 2. 解析配置 ----
        config_parse(g_conf_buf, conf_len, cpuinfo_len ? g_cpuinfo_buf : nullptr, cpuinfo_len);
        if (!config_loaded()) {
            LOGE("配置解析失败, 放弃伪装");
            return;
        }

        // ---- 3. 真正的名单过滤(配置已就绪) ----
        if (!SpoofDb::instance().app_matches(process_name)) {
            LOGD("%s 不在伪装名单里, 跳过", process_name);
            return;
        }
        LOGI("命中目标进程: %s", process_name);

        // ---- 4. 安装所有 hook ----
        plt_hook_reset();
        hook_resolve_originals();
        hook_install_property();
        if (options().hook_property_find) hook_install_property_find();
        hook_install_file_overlay();
        hook_install_misc();
        plt_hook_apply();

        armed_ = true;
        LOGI("伪装已就绪: 属性 %zu 条 / 虚拟文件 %zu 个", SpoofDb::instance().property_count(),
             SpoofDb::instance().text_count());
    }

    void postAppSpecialize(const AppSpecializeArgs *) override {
        if (!armed_) return;

        // Java 层: android.os.Build / SystemProperties 最终都走这里
        hook_install_jni(env_, api_);

        LOGI("进程 %d 伪装完成", static_cast<int>(getpid()));
    }

private:
    Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool armed_ = false;
};

} // namespace

REGISTER_ZYGISK_MODULE(Mate80ProModule)

REGISTER_ZYGISK_COMPANION(hw80::companion_handler)
