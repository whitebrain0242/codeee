#pragma once

#include "minimuduo/net/Callbacks.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class OnlineUserRegistry {
public:
    using TcpConnectionPtr =
        minimuduo::net::TcpConnectionPtr;

    bool add(
        const std::string& username,
        const TcpConnectionPtr& connection
    );

    void remove(
        const std::string& username,
        const TcpConnectionPtr& connection
    );

    bool find(
        const std::string& username,
        TcpConnectionPtr& connection
    );

    bool is_online(
        const std::string& username
    );

    std::vector<std::string>
    usernames();

    std::vector<TcpConnectionPtr>
    connections(
        const TcpConnectionPtr& except = {}
    );

private:
    void prune_locked();

    std::mutex mutex_;

    std::unordered_map<
        std::string,
        std::weak_ptr<
            minimuduo::net::TcpConnection
        >
    > users_;
};
