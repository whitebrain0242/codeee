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
}
