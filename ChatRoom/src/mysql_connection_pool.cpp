#include "integration/mysql_connection_pool.hpp"

#include <chrono>

namespace {

MYSQL* open_connection(
    const MySqlConfig& config,
    std::string& error
) {
    MYSQL* connection =
        mysql_init(nullptr);

    if (connection == nullptr) {
        error = "mysql_init failed";
        return nullptr;
    }

    mysql_options(
        connection,
        MYSQL_OPT_CONNECT_TIMEOUT,
        &config.connect_timeout_seconds
    );

    if (mysql_real_connect(
            connection,
            config.host.c_str(),
            config.user.c_str(),
            config.password.c_str(),
            config.database.c_str(),
            config.port,
            nullptr,
            0
        ) == nullptr) {
        error = mysql_error(connection);
        mysql_close(connection);
        return nullptr;
    }

    if (mysql_set_character_set(
            connection,
            "utf8mb4"
        ) != 0) {
        error = mysql_error(connection);
        mysql_close(connection);
        return nullptr;
    }

    return connection;
}

}  // namespace

MySqlConnectionPool::Lease::Lease(
    MySqlConnectionPool* owner,
    MYSQL* connection
)
    : owner_(owner),
      connection_(connection) {}

MySqlConnectionPool::Lease::~Lease() {
    reset();
}

MySqlConnectionPool::Lease::Lease(
    Lease&& other
) noexcept
    : owner_(other.owner_),
      connection_(other.connection_) {
    other.owner_ = nullptr;
    other.connection_ = nullptr;
}

MySqlConnectionPool::Lease&
MySqlConnectionPool::Lease::operator=(
    Lease&& other
) noexcept {
    if (this == &other) {
        return *this;
    }

    reset();

    owner_ = other.owner_;
    connection_ = other.connection_;

    other.owner_ = nullptr;
    other.connection_ = nullptr;
    return *this;
}

MYSQL*
MySqlConnectionPool::Lease::get() const noexcept {
    return connection_;
}

MySqlConnectionPool::Lease::operator bool() const noexcept {
    return connection_ != nullptr;
}

void MySqlConnectionPool::Lease::reset() noexcept {
    if (owner_ != nullptr &&
        connection_ != nullptr) {
        owner_->release(connection_);
    }

    owner_ = nullptr;
    connection_ = nullptr;
}

MySqlConnectionPool::~MySqlConnectionPool() {
    shutdown();
}

bool MySqlConnectionPool::initialize(
    const MySqlConfig& config,
    std::string& error
) {
    shutdown();

    std::vector<MYSQL*> opened;
    opened.reserve(config.pool_size);

    for (unsigned int i = 0U;
         i < config.pool_size;
         ++i) {
        MYSQL* connection =
            open_connection(
                config,
                error
            );

        if (connection == nullptr) {
            for (MYSQL* item : opened) {
                mysql_close(item);
            }

            error =
                "MySQL pool initialization failed at connection " +
                std::to_string(i + 1U) +
                "/" +
                std::to_string(config.pool_size) +
                ": " +
                error;

            return false;
        }

        opened.push_back(connection);
    }

    {
        std::lock_guard<std::mutex> lock(
            mutex_
        );

        stopping_ = false;
        all_ = opened;

        for (MYSQL* connection : opened) {
            available_.push_back(
                connection
            );
        }
    }

    return true;
}

MySqlConnectionPool::Lease
MySqlConnectionPool::acquire(
    std::string& error
) {
    std::unique_lock<std::mutex> lock(
        mutex_
    );

    const bool ready =
        cv_.wait_for(
            lock,
            std::chrono::seconds(5),
            [this] {
                return
                    stopping_ ||
                    !available_.empty();
            }
        );

    if (!ready) {
        error =
            "timed out waiting for a MySQL pool connection";
        return {};
    }

    if (stopping_ ||
        available_.empty()) {
        error =
            "MySQL connection pool is stopping";
        return {};
    }

    MYSQL* connection =
        available_.front();

    available_.pop_front();

    return Lease(
        this,
        connection
    );
}

std::size_t
MySqlConnectionPool::size() const noexcept {
    std::lock_guard<std::mutex> lock(
        mutex_
    );

    return all_.size();
}

void MySqlConnectionPool::shutdown() {
    std::vector<MYSQL*> connections;

    {
        std::unique_lock<std::mutex> lock(
            mutex_
        );

        if (all_.empty()) {
            stopping_ = true;
            available_.clear();
            return;
        }

        stopping_ = true;
        cv_.notify_all();

        cv_.wait(
            lock,
            [this] {
                return
                    available_.size() ==
                    all_.size();
            }
        );

        connections.swap(all_);
        available_.clear();
    }

    for (MYSQL* connection :
         connections) {
        mysql_close(connection);
    }
}

void MySqlConnectionPool::release(
    MYSQL* connection
) noexcept {
    if (connection == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(
            mutex_
        );

        available_.push_back(
            connection
        );
    }

    cv_.notify_one();
}
