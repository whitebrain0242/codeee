#include "password.hpp"

#include <iostream>
#include <string>

int main() {
    const std::string encoded = hash_password_pbkdf2("pass123");

    if (encoded.find("pbkdf2_sha256$") != 0) {
        std::cerr << "unexpected password format\n";
        return 1;
    }
    if (!verify_password_pbkdf2("pass123", encoded)) {
        std::cerr << "correct password rejected\n";
        return 1;
    }
    if (verify_password_pbkdf2("wrong", encoded)) {
        std::cerr << "wrong password accepted\n";
        return 1;
    }

    std::cout << "password tests passed\n";
    return 0;
}
