// 与 root companion 进程之间的协议。
//
// 为什么需要 companion:
//   preAppSpecialize 跑在 zygote 的安全上下文里, 按 SELinux 策略不一定能读
//   /data/adb 下的文件。companion 进程以 root 运行, 由它读配置再通过 unix
//   socket 传回来, 这样无论 ROM 的 SELinux 策略怎么收紧都不会失败。
//
// 协议(小端, 无对齐要求):
//   客户端 -> 服务端: 1 字节 命令
//   服务端 -> 客户端: 4 字节 payload 长度
//                     4 字节 cpuinfo 模板长度 (0 表示"用编译内置的")
//                     payload  (即 hw80pro.conf 的完整内容)
//                     模板内容
//   长度为 0 且 payload 为空 = 读不到配置, 客户端使用内置默认值。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hw80 {

constexpr uint8_t kCompanionCmdLoadConfig = 1;
constexpr size_t kCompanionMaxPayload = 512 * 1024;

/// companion 进程侧: 处理一次请求(socket 由 Zygisk 传入)。
void companion_handler(int socket_fd);

/// 应用进程侧: 通过 socket 取回配置。成功返回 true。
/// conf_out/cpuinfo_out 指向静态缓冲区, 长度写入 len。
bool companion_fetch(int socket_fd, char *conf_out, size_t conf_cap, size_t *conf_len,
                     char *cpuinfo_out, size_t cpuinfo_cap, size_t *cpuinfo_len);

} // namespace hw80
