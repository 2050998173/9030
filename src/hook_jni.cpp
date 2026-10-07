// Java 层属性拦截。
//
// android.os.SystemProperties 的 get/getInt/getLong/getBoolean 最终落到
// libandroid_runtime.so 注册的 native 方法:
//   native_get(String,String)           -> String
//   native_get_int(String,int)          -> int
//   native_get_long(String,long)        -> long
//   native_get_boolean(String,boolean)  -> boolean
//
// 用 Zygisk 的 hookJniNativeMethods 替换它们, 同时把原始函数指针拿回来
// (api 会把原实现写到 JNINativeMethod.fnPtr 里), 这样未命中的属性仍走原实现。
//
// android.os.Build.* 在 Android 13+ 是惰性读取的, 所以在这个时机替换有效。
//
// 注意: 这些方法带 @FastNative 注解。FastNative 不改变 JNI 调用约定, 只影响
// ART 的入口检查, 所以用同样的签名替换是安全的。
#include <jni.h>
#include <stdint.h>

#include "config.hpp"
#include "hooks.hpp"
#include "spoof_db.hpp"
#include "util.hpp"
#include "zygisk.hpp"

namespace hw80 {
namespace {

jclass g_sp_cls = nullptr;

using GetSS = jstring (*)(JNIEnv *, jclass, jstring, jstring);
using GetI = jint (*)(JNIEnv *, jclass, jstring, jint);
using GetL = jlong (*)(JNIEnv *, jclass, jstring, jlong);
using GetZ = jboolean (*)(JNIEnv *, jclass, jstring, jboolean);

GetSS g_orig_get_ss = nullptr;
GetI g_orig_get_i = nullptr;
GetL g_orig_get_l = nullptr;
GetZ g_orig_get_z = nullptr;

bool key_from_jstring(JNIEnv *env, jstring key, char *out, size_t cap) {
    out[0] = '\0';
    if (!env || !key || cap == 0) return false;
    const char *utf = env->GetStringUTFChars(key, nullptr);
    if (!utf) return false;
    copy_to_buf(out, cap, utf);
    env->ReleaseStringUTFChars(key, utf);
    return true;
}

// --- 替换实现。签名必须与 java 侧声明完全一致 ---
jstring hf_native_get(JNIEnv *env, jclass cls, jstring jkey, jstring jdef) {
    char key[256];
    if (key_from_jstring(env, jkey, key, sizeof(key))) {
        if (const char *v = SpoofDb::instance().get_property(key, nullptr)) {
            LOGD("SystemProperties.native_get(%s) -> %s", key, v);
            return env->NewStringUTF(v);
        }
    }
    if (g_orig_get_ss) return g_orig_get_ss(env, cls, jkey, jdef);
    return jdef;
}

jint hf_native_get_int(JNIEnv *env, jclass cls, jstring jkey, jint def) {
    char key[256];
    if (key_from_jstring(env, jkey, key, sizeof(key))) {
        if (const char *v = SpoofDb::instance().get_property(key, nullptr)) {
            LOGD("SystemProperties.native_get_int(%s) -> %s", key, v);
            return static_cast<jint>(parse_long(v, def));
        }
    }
    if (g_orig_get_i) return g_orig_get_i(env, cls, jkey, def);
    return def;
}

jlong hf_native_get_long(JNIEnv *env, jclass cls, jstring jkey, jlong def) {
    char key[256];
    if (key_from_jstring(env, jkey, key, sizeof(key))) {
        if (const char *v = SpoofDb::instance().get_property(key, nullptr))
            return static_cast<jlong>(parse_long(v, static_cast<long>(def)));
    }
    if (g_orig_get_l) return g_orig_get_l(env, cls, jkey, def);
    return def;
}

jboolean hf_native_get_boolean(JNIEnv *env, jclass cls, jstring jkey, jboolean def) {
    char key[256];
    if (key_from_jstring(env, jkey, key, sizeof(key))) {
        if (const char *v = SpoofDb::instance().get_property(key, nullptr))
            return parse_bool(v) ? JNI_TRUE : JNI_FALSE;
    }
    if (g_orig_get_z) return g_orig_get_z(env, cls, jkey, def);
    return def;
}

} // namespace

void hook_install_jni(JNIEnv *env, zygisk::Api *api) {
    if (!env) {
        LOGE("JNIEnv 为空, 无法替换 Java 层属性读取");
        return;
    }
    if (!api) {
        LOGE("Zygisk API 不可用, 无法替换 Java 层属性读取");
        return;
    }

    jclass local = env->FindClass("android/os/SystemProperties");
    if (!local) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("找不到 android.os.SystemProperties, 跳过 Java 层拦截");
        return;
    }
    g_sp_cls = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (!g_sp_cls) {
        LOGE("NewGlobalRef(SystemProperties) 失败");
        return;
    }

    // 逐条检查签名是否存在, 避免把不存在的签名注册进去。
    struct Candidate {
        const char *name;
        const char *sig;
        void *replacement;
    };
    const Candidate candidates[] = {
        {"native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
         reinterpret_cast<void *>(&hf_native_get)},
        {"native_get_int", "(Ljava/lang/String;I)I", reinterpret_cast<void *>(&hf_native_get_int)},
        {"native_get_long", "(Ljava/lang/String;J)J",
         reinterpret_cast<void *>(&hf_native_get_long)},
        {"native_get_boolean", "(Ljava/lang/String;Z)Z",
         reinterpret_cast<void *>(&hf_native_get_boolean)},
    };

    JNINativeMethod methods[4]{};
    int n = 0;
    for (const Candidate &c : candidates) {
        // GetStaticMethodID 失败时返回 nullptr 【并且】抛出 NoSuchMethodError,
        // 所以必须用 mid 本身判断, 不能把"有异常"当成"方法存在"。
        jmethodID mid = env->GetStaticMethodID(g_sp_cls, c.name, c.sig);
        const bool exists = (mid != nullptr);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!exists) {
            LOGW("SystemProperties.%s%s 不存在, 跳过", c.name, c.sig);
            continue;
        }
        methods[n].name = const_cast<char *>(c.name);
        methods[n].signature = const_cast<char *>(c.sig);
        methods[n].fnPtr = c.replacement;
        ++n;
    }

    if (n == 0) {
        LOGE("SystemProperties 的 native 方法一个都没匹配上, Java 层拦截失败");
        return;
    }

    // hookJniNativeMethods 会把原实现写回到 fnPtr 里
    api->hookJniNativeMethods(env, "android/os/SystemProperties", methods, n);

    int hooked = 0;
    for (int i = 0; i < n; ++i) {
        void *orig = methods[i].fnPtr;
        if (!orig) {
            LOGW("SystemProperties.%s 替换失败(未找到已注册的 native)", methods[i].name);
            continue;
        }
        if (str_eq(methods[i].name, "native_get")) g_orig_get_ss = reinterpret_cast<GetSS>(orig);
        else if (str_eq(methods[i].name, "native_get_int"))
            g_orig_get_i = reinterpret_cast<GetI>(orig);
        else if (str_eq(methods[i].name, "native_get_long"))
            g_orig_get_l = reinterpret_cast<GetL>(orig);
        else if (str_eq(methods[i].name, "native_get_boolean"))
            g_orig_get_z = reinterpret_cast<GetZ>(orig);
        ++hooked;
    }

    if (hooked > 0)
        LOGI("Java 层属性拦截已生效: %d/%d 个 native 方法", hooked, n);
    else
        LOGE("Java 层属性拦截没有生效(可能类加载时机不对)");
}

} // namespace hw80
