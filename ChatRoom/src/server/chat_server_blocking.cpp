#include "chat_server.hpp"
#include "minimuduo/net/TcpConnection.hpp"

void ChatServer::handle_block_friend(
    const TcpConnectionPtr& connection,
    const ClientSession& session,
    const std::string& arguments
) {
    if (!require_login(
            connection,
            session,
            "blocking friend messages"
        )) {
        return;
    }

    std::string target;

    if (!extract_single_username(
            connection,
            arguments,
            "BLOCK_FRIEND <username>",
            target
        )) {
        return;
    }

    if (target == session.username) {
        connection->send(
            "[error] you cannot block yourself.\n"
        );
        return;
    }

    std::string error;
    bool created = false;

    {
        std::lock_guard<std::mutex> operation_lock(
            friend_operation_mutex_
        );

        bool friends = false;

        if (!database_.are_friends(
                session.username,
                target,
                friends,
                error
            )) {
            database_error(
                connection,
                "checking friendship before blocking",
                error
            );
            return;
        }

        if (!friends) {
            connection->send(
                "[error] only an existing friend "
                "can be message-blocked.\n"
            );
            return;
        }

        if (!database_.add_friend_block(
                session.username,
                target,
                created,
                error
            )) {
            database_error(
                connection,
                "blocking friend messages",
                error
            );
            return;
        }
    }

    if (!created) {
        connection->send(
            "[system] " +
            target +
            " is already blocked from sending "
            "you direct messages/files.\n"
        );
        return;
    }

    connection->send(
        "[system] blocked direct messages/files from " +
        target +
        ". Friendship, old history, public chat and "
        "group chat are unchanged.\n"
        "[system] pending direct items from this friend "
        "remain stored but are not delivered while blocked.\n"
    );
}

void ChatServer::handle_unblock_friend(
    const TcpConnectionPtr& connection,
    const ClientSession& session,
    const std::string& arguments
) {
    if (!require_login(
            connection,
            session,
            "unblocking friend messages"
        )) {
        return;
    }

    std::string target;

    if (!extract_single_username(
            connection,
            arguments,
            "UNBLOCK_FRIEND <username>",
            target
        )) {
        return;
    }

    bool removed = false;
    std::string error;

    {
        std::lock_guard<std::mutex> operation_lock(
            friend_operation_mutex_
        );

        if (!database_.remove_friend_block(
                session.username,
                target,
                removed,
                error
            )) {
            database_error(
                connection,
                "unblocking friend messages",
                error
            );
            return;
        }
    }

    if (!removed) {
        connection->send(
            "[error] " +
            target +
            " is not in your blocked-friend list.\n"
        );
        return;
    }

    connection->send(
        "[system] unblocked direct messages/files from " +
        target +
        ". Use PENDING to immediately retry "
        "stored offline deliveries.\n"
    );
}

void ChatServer::handle_blocked_friends(
    const TcpConnectionPtr& connection,
    const ClientSession& session
) {
    if (!require_login(
            connection,
            session,
            "viewing blocked friends"
        )) {
        return;
    }

    std::vector<std::string> blocked;
    std::string error;

    if (!database_.list_blocked_friends(
            session.username,
            blocked,
            error
        )) {
        database_error(
            connection,
            "listing blocked friends",
            error
        );
        return;
    }

    connection->send(
        "[system] blocked friends (" +
        std::to_string(
            blocked.size()
        ) +
        "): " +
        join_names(blocked) +
        "\n"
    );
}

