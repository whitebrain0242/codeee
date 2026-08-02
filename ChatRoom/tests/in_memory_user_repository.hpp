#pragma once

#include "user_repository.hpp"

#include <string>
#include <unordered_map>

namespace chat::test {

class InMemoryUserRepository final
    : public IUserRepository {
public:
    bool initialize(
        std::string& error
    ) override;

    bool load_usernames(
        std::vector<std::string>& usernames,
        std::string& error
    ) override;

    CreateUserResult create_user(
        const std::string& username,
        const std::string& password,
        std::string& error
    ) override;

    VerifyUserResult verify_user(
        const std::string& username,
        const std::string& password,
        std::string& error
    ) override;

private:
    std::unordered_map<
        std::string,
        std::string
    > users_;
};

}  // namespace chat::test
