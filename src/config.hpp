// 模块运行期选项 + 配置文件解析 + CPU 型号名运行时替换。
//
// 数据来源优先级:
//   1) /data/adb/hw80pro/hw80pro.conf          (用户可改)
//   2) 编译进 so 的默认配置                      (config/hw80pro.conf)
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hw80 {

/// 与 companion 共享的默认路径。companion 在 root 域读取这些文件并通过
/// socket 传回, 因此不依赖 zygote/app 进程的 SELinux 权限。
namespace paths {
inline constexpr const char *kModuleDataDir = "/data/adb/hw80pro";
inline constexpr const char *kConfPath = "/data/adb/hw80pro/hw80pro.conf";
inline constexpr const char *kDefaultCpuInfoPath = "/data/adb/hw80pro/proc_cpuinfo.txt";
} // namespace paths

struct Options {
    bool debug = false;
    bool hook_property_find = true;
    bool hook_exec_getprop = true;
    bool patch_sys_cpu = true;
    bool spoof_uname = false;
    bool have_mem_total = false;
};

/// uname 伪装字段。用固定数组保存, 这样 hook_misc 不需要依赖属性表
/// (uname_* 这类键名不含 '.', 不能作为系统属性存储)。
struct UnameConfig {
    char sysname[64] = "Linux";
    char nodename[64] = "localhost";
    char release[128] = "5.10.209-android13-8-g2f7c1b9a1c5e-ab12345678";
    char version[192] = "#1 SMP PREEMPT Thu Nov 20 10:23:41 CST 2025";
    char machine[64] = "aarch64";
    char domainname[64] = "";
};

Options &options();
UnameConfig &uname_config();

/// 解析配置文本, 填充 SpoofDb / Options。
/// cpuinfo_text / cpuinfo_text_len 是"可选的" /proc/cpuinfo 模板内容(通常由
/// companion 从 /data/adb/hw80pro/proc_cpuinfo.txt 读来); 传空则使用编译进
/// so 的默认模板。
void config_parse(const char *text, size_t len, const char *cpuinfo_text,
                  size_t cpuinfo_text_len);

/// 把 cpuinfo 模板里的型号名/核数替换成伪装值(用于 cpuinfo=@device 模式)。
/// 返回写入 out 的长度。
size_t cpuinfo_mask_device(const char *in, size_t in_len, char *out, size_t out_cap);

/// 当前是否已成功加载伪装数据(供 zygisk 模块判断要不要继续)。
bool config_loaded();

} // namespace hw80
