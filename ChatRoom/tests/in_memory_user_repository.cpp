#include "in_memory_user_repository.hpp"

#include <algorithm>

namespace chat::test {

bool InMemoryUserRepository::initialize(
    std::string& error
) {
    error.clear();
    return true;
}

bool InMemoryUserRepository::load_usernames(
    std::vector<std::string>& usernames,
    std::string& error
) {
    error.clear();
    usernames.clear();
    usernames.reserve(users_.size());

    for (const auto& [username, password] :
         users_) {
        (void)password;
        usernames.push_back(username);
    }

    std::sort(
        usernames.begin(),
        usernames.end()
    );

    return true;
}

CreateUserResult
InMemoryUserRepository::create_user(
    const std::string& username,
    const std::string& password,
    std::string& error
) {
    error.clear();

    const auto [iterator, inserted] =
        users_.emplace(username, password);

    (void)iterator;

    return inserted
        ? CreateUserResult::Success
        : CreateUserResult::AlreadyExists;
}

VerifyUserResult
InMemoryUserRepository::verify_user(
    const std::string& username,
    const std::string& password,
    std::string& error
) {
    error.clear();

    const auto it = users_.find(username);

    if (
        it == users_.end() ||
        it->second != password
    ) {
        return VerifyUserResult::InvalidCredentials;
    }

    return VerifyUserResult::Success;
}

}  // namespace chat::test
