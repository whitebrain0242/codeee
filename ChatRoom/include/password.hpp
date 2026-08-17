#pragma once

#include <string>

std::string hash_password_pbkdf2(const std::string& password);
bool verify_password_pbkdf2(const std::string& password, const std::string& encoded);