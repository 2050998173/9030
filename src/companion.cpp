// root companion 进程: 读配置文件并回传给目标进程。
//
// 这个函数运行在独立的 root 守护进程里, 可以为多个目标进程并发调用,
// 所以只使用局部变量, 不碰任何全局可变状态。
#include "companion.hpp"

#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "config.hpp"
#include "util.hpp"

namespace hw80 {
namespace {

ssize_t sock_read(int fd, void *buf, size_t len) {
    return ::recv(fd, buf, len, 0);
}

bool sock_read_all(int fd, void *buf, size_t len) {
    auto *p = static_cast<uint8_t *>(buf);
    size_t got = 0;
    while (got < len) {
        const ssize_t n = sock_read(fd, p + got, len - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool sock_write_all(int fd, const void *buf, size_t len) {
    const auto *p = static_cast<const uint8_t *>(buf);
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void write_u32(uint8_t *dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xff);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint32_t read_u32(const uint8_t *src) {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

} // namespace

void companion_handler(int socket_fd) {
    if (socket_fd < 0) return;

    uint8_t cmd = 0;
    if (!sock_read_all(socket_fd, &cmd, 1)) {
        ::close(socket_fd);
        return;
    }

    if (cmd != kCompanionCmdLoadConfig) {
        const uint8_t zero[8] = {};
        sock_write_all(socket_fd, zero, sizeof(zero));
        ::close(socket_fd);
        return;
    }

    // 配置文件。缓冲区放静态存储, 避免在栈上开 512KB。
    // companion 进程串行处理请求, 因此静态缓冲区不会互相干扰。
    static char conf[kCompanionMaxPayload];
    long conf_len = sys_read_file(paths::kConfPath, conf, sizeof(conf));
    if (conf_len < 0) conf_len = 0;

    // 可选的 cpuinfo 模板。存在就一起传过去, 这样应用进程不需要再读文件。
    static char cpuinfo[64 * 1024];
    long cpuinfo_len = sys_read_file(paths::kDefaultCpuInfoPath, cpuinfo, sizeof(cpuinfo));
    if (cpuinfo_len < 0) cpuinfo_len = 0;

    uint8_t header[8];
    write_u32(header, static_cast<uint32_t>(conf_len));
    write_u32(header + 4, static_cast<uint32_t>(cpuinfo_len));
    if (!sock_write_all(socket_fd, header, sizeof(header))) {
        ::close(socket_fd);
        return;
    }
    if (conf_len > 0 && !sock_write_all(socket_fd, conf, static_cast<size_t>(conf_len))) {
        ::close(socket_fd);
        return;
    }
    if (cpuinfo_len > 0 &&
        !sock_write_all(socket_fd, cpuinfo, static_cast<size_t>(cpuinfo_len))) {
        ::close(socket_fd);
        return;
    }

    ::close(socket_fd);
}

bool companion_fetch(int socket_fd, char *conf_out, size_t conf_cap, size_t *conf_len,
                     char *cpuinfo_out, size_t cpuinfo_cap, size_t *cpuinfo_len) {
    if (socket_fd < 0 || !conf_out || !conf_len) return false;
    *conf_len = 0;
    if (cpuinfo_len) *cpuinfo_len = 0;

    const uint8_t cmd = kCompanionCmdLoadConfig;
    if (!sock_write_all(socket_fd, &cmd, 1)) return false;

    uint8_t header[8];
    if (!sock_read_all(socket_fd, header, sizeof(header))) return false;

    const uint32_t conf_size = read_u32(header);
    const uint32_t cpuinfo_size = read_u32(header + 4);
    // 必须留出结尾 '\0' 的位置, 因此要求严格小于容量
    if (conf_size >= conf_cap) return false;
    if (cpuinfo_out && cpuinfo_size >= cpuinfo_cap) return false;

    if (conf_size > 0) {
        if (!sock_read_all(socket_fd, conf_out, conf_size)) return false;
        *conf_len = conf_size;
    }
    conf_out[*conf_len] = '\0';

    if (cpuinfo_out && cpuinfo_size > 0) {
        if (!sock_read_all(socket_fd, cpuinfo_out, cpuinfo_size)) return false;
        if (cpuinfo_len) *cpuinfo_len = cpuinfo_size;
        cpuinfo_out[cpuinfo_size] = '\0';
    }
    return true;
}

} // namespace hw80
