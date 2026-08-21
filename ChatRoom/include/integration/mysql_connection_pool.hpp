#pragma once

#include "config.hpp"

#include <mysql/mysql.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class MySqlConnectionPool {
public:
    class Lease {
    public:
        Lease() = default;
        ~Lease();

        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        MYSQL* get() const noexcept;
        explicit operator bool() const noexcept;

    private:
        friend class MySqlConnectionPool;

        Lease(
            MySqlConnectionPool* owner,
            MYSQL* connection
        );

        void reset() noexcept;

        MySqlConnectionPool* owner_ = nullptr;
        MYSQL* connection_ = nullptr;
    };

    MySqlConnectionPool() = default;
    ~MySqlConnectionPool();

    MySqlConnectionPool(
        const MySqlConnectionPool&
    ) = delete;

    MySqlConnectionPool& operator=(
        const MySqlConnectionPool&
    ) = delete;

    bool initialize(
        const MySqlConfig& config,
        std::string& error
    );

    Lease acquire(
        std::string& error
    );

    std::size_t size() const noexcept;

    void shutdown();

private:
    friend class Lease;

    void release(
        MYSQL* connection
    ) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::vector<MYSQL*> all_;
    std::deque<MYSQL*> available_;

    bool stopping_ = false;
};
