#pragma once
#include <string>
namespace minimuduo::net{
bool configureTcpKeepAlive(
    int socketFd,
    int idleSeconds,
    int intervalSeconds,
    int probeCount,
    std::string& error
);

// 聊天消息是大量小包，关闭 Nagle 可显著降低交互延迟。
bool configureTcpNoDelay(
    int socketFd,
    bool enabled,
    std::string& error
);
}
