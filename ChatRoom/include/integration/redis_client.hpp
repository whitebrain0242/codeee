#pragma once

#include "config.hpp"

#include <hiredis/hiredis.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

struct RedisUnreadCounts {
    std::int64_t private_messages = 0;
    std::int64_t group_messages = 0;
    std::int64_t private_files = 0;
    std::int64_t group_files = 0;
};

class RedisClient {
public:
    RedisClient() = default;
    ~RedisClient();

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    bool connect(
        const RedisConfig& config,
        std::string& error
    );

    bool ping(std::string& error);

    bool claim_presence(
        const std::string& username,
        const std::string& server_instance_id,
        unsigned int ttl_seconds,
        bool& claimed,
        std::string& error
    );

    bool refresh_presence_if_owned(
        const std::string& username,
        const std::string& server_instance_id,
        unsigned int ttl_seconds,
        bool& refreshed,
        std::string& error
    );

    bool remove_presence_if_owned(
        const std::string& username,
        const std::string& server_instance_id,
        bool& removed,
        std::string& error
    );

    bool presence_owner(
        const std::string& username,
        std::optional<std::string>& owner,
        std::string& error
    );

    bool adjust_unread(
        const std::string& username,
        const std::string& kind,
        std::int64_t delta,
        std::int64_t& result,
        std::string& error
    );

    bool unread_counts(
        const std::string& username,
        RedisUnreadCounts& counts,
        std::string& error
    );

private:
    RedisConfig config_;
    redisContext* context_ = nullptr;
    std::mutex mutex_;

    bool connect_locked(std::string& error);
    bool ensure_connected_locked(std::string& error);
    bool authenticate_and_select_locked(std::string& error);
    void close_locked();

    std::string presence_key(
        const std::string& username
    ) const;

    std::string unread_key(
        const std::string& username
    ) const;

    static bool is_supported_unread_kind(
        const std::string& kind
    );
};
