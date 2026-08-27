#include "minimuduo/net/SocketOptions.hpp"

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
//心跳检测
namespace minimuduo::net {
namespace {
bool setIntOption(int socketFd,
                  int level,        // 改哪一个层的选项
                  int option,       // 具体哪一个开关
                  int value,        // 对于开关类参数，改成什么样
                  const char *name, // 参数的名字
                  std::string &error) {
  if (::setsockopt(socketFd, level, option, &value, sizeof(value)) != 0) {
    error = std::string(name) + " 设置失败：" + std::strerror(errno);
    return false;
  }
  return true;
}
} // namespace
// 把上层传来的参数经过多次调用，逐步写入操作系统TCP协议栈，让内核帮助看这个连接是否还活着
bool configureTcpKeepAlive(int socketFd,
                           int idleSeconds,     // 空闲等待时间
                           int intervalSeconds, // 探测重试间隔
                           int probeCount,      // 错误上限
                           std::string &error) {
  if (!setIntOption(socketFd, SOL_SOCKET, SO_KEEPALIVE, 1, "SO_KEEPALIVE",
                    error))
    return false;

  if (!setIntOption(socketFd, IPPROTO_TCP, TCP_KEEPINTVL, intervalSeconds,
                    "TCP_KEEPINTVL", error))
    return false;

  if (!setIntOption(socketFd, IPPROTO_TCP, TCP_KEEPIDLE, idleSeconds,
                    "TCP_KEEPIDLE", error))
    return false;

  if (!setIntOption(socketFd, IPPROTO_TCP, TCP_KEEPCNT, probeCount,
                    "TCP_KEEPCNT", error))
    return false;

  return true;
}
bool configureTcpNoDelay(int socketFd, bool enabled, std::string &error) {
  return setIntOption(socketFd, IPPROTO_TCP, TCP_NODELAY, enabled ? 1 : 0,
                      "TCP_NODELAY", error);
}

} // namespace minimuduo::net