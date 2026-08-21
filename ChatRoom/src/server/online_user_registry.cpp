#include "server/online_user_registry.hpp"

#include "minimuduo/net/TcpConnection.hpp"

#include <algorithm>

bool OnlineUserRegistry::add(
    const std::string& username,
    const TcpConnectionPtr& connection
) {
    std::lock_guard<std::mutex> lock(
        mutex_
    );

    const auto iterator =
        users_.find(username);

    if (iterator != users_.end()) {
        const TcpConnectionPtr current =
            iterator->second.lock();

        if (current != nullptr &&
            current->connected()) {
            return false;
        }

        users_.erase(iterator);
    }

    users_[username] =
        connection;

    return true;
}

void OnlineUserRegistry::remove(
    const std::string& username,
    const TcpConnectionPtr& connection
) {
    std::lock_guard<std::mutex> lock(
        mutex_
    );

    const auto iterator =
        users_.find(username);

    if (iterator == users_.end()) {
        return;
    }

    const TcpConnectionPtr current =
        iterator->second.lock();

    if (current == nullptr ||
        current == connection) {
        users_.erase(iterator);
    }
}

bool OnlineUserRegistry::find(
    const std::string& username,
    TcpConnectionPtr& connection
) {
    std::lock_guard<std::mutex> lock(
        mutex_
    );

    const auto iterator =
        users_.find(username);

    if (iterator == users_.end()) {
        connection.reset();
        return false;
    }

    connection =
        iterator->second.lock();

    if (connection == nullptr ||
        !connection->connected()) {
        users_.erase(iterator);
        connection.reset();
        return false;
    }

    return true;
}

bool OnlineUserRegistry::is_online(
    const std::string& username
) {
    TcpConnectionPtr connection;

    return find(
        username,
        connection
    );
}

std::vector<std::string>
OnlineUserRegistry::usernames() {
    std::lock_guard<std::mutex> lock(
        mutex_
    );

    prune_locked();

    std::vector<std::string> names;
    names.reserve(users_.size());

    for (const auto& item : users_) {
        names.push_back(item.first);
    }

    std::sort(
        names.begin(),
        names.end()
    );

    return names;
}

std::vector<
    OnlineUserRegistry::TcpConnectionPtr
>
OnlineUserRegistry::connections(
    const TcpConnectionPtr& except
) {
    std::lock_guard<std::mutex> lock(
        mutex_
    );

    std::vector<TcpConnectionPtr>
        result;

    for (auto iterator = users_.begin();
         iterator != users_.end();) {
        TcpConnectionPtr connection =
            iterator->second.lock();

        if (connection == nullptr ||
            !connection->connected()) {
            iterator =
                users_.erase(iterator);
            continue;
        }

        if (connection != except) {
            result.push_back(
                std::move(connection)
            );
        }

        ++iterator;
    }

    return result;
}

void OnlineUserRegistry::prune_locked() {
    for (auto iterator = users_.begin();
         iterator != users_.end();) {
        const TcpConnectionPtr connection =
            iterator->second.lock();

        if (connection == nullptr ||
            !connection->connected()) {
            iterator =
                users_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}
