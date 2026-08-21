#pragma once

#include "mysql_database.hpp"

#include <string>

enum class DirectMessageDecision {
    Allowed,
    TargetMissing,
    NotFriends,
    BlockedByRecipient,
    DatabaseError
};

class DirectMessagePolicy {
public:
    explicit DirectMessagePolicy(
        MySqlDatabase& database
    )
        : database_(database) {}

    DirectMessageDecision evaluate(
        const std::string& sender,
        const std::string& recipient,
        std::string& error
    );

private:
    MySqlDatabase& database_;
};
