#include "password_hasher.hpp"

#include <iostream>
#include <string>

namespace {

bool expect(
    bool condition,
    const std::string& name
) {
    std::cout
        << name
        << ": "
        << (condition ? "PASS" : "FAIL")
        << '\n';

    return condition;
}

}  // namespace

int main() {
    chat::PasswordRecord first;
    chat::PasswordRecord second;
    std::string error;

    if (
        !expect(
            chat::PasswordHasher::create(
                "correct-password",
                first,
                error
            ),
            "create first password record"
        )
    ) {
        std::cerr << error << std::endl;
        return 1;
    }

    if (
        !expect(
            chat::PasswordHasher::create(
                "correct-password",
                second,
                error
            ),
            "create second password record"
        )
    ) {
        std::cerr << error << std::endl;
        return 1;
    }

    if (
        !expect(
            first.salt != second.salt,
            "same password gets different salt"
        )
    ) {
        return 1;
    }

    bool matches = false;

    if (
        !expect(
            chat::PasswordHasher::verify(
                "correct-password",
                first,
                matches,
                error
            ) && matches,
            "correct password verifies"
        )
    ) {
        std::cerr << error << std::endl;
        return 1;
    }

    matches = true;

    if (
        !expect(
            chat::PasswordHasher::verify(
                "wrong-password",
                first,
                matches,
                error
            ) && !matches,
            "wrong password rejected"
        )
    ) {
        std::cerr << error << std::endl;
        return 1;
    }

    if (
        !expect(
            first.salt.size() ==
                chat::PasswordHasher::kSaltSize,
            "salt size"
        )
    ) {
        return 1;
    }

    if (
        !expect(
            first.hash.size() ==
                chat::PasswordHasher::kHashSize,
            "hash size"
        )
    ) {
        return 1;
    }

    return 0;
}
