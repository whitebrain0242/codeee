cmake_minimum_required(VERSION 3.16)

project(chatroom_v7_1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

pkg_check_modules(
    MYSQLCLIENT
    QUIET
    IMPORTED_TARGET
    mysqlclient
)

if(NOT MYSQLCLIENT_FOUND)
    pkg_check_modules(
        MYSQLCLIENT
        REQUIRED
        IMPORTED_TARGET
        mariadb
    )
endif()

add_library(chat_common
    src/protocol.cpp
    src/message_store.cpp
)

target_include_directories(
    chat_common
    PUBLIC include
)

add_library(chat_security
    src/password_hasher.cpp
)

target_include_directories(
    chat_security
    PUBLIC include
)

target_link_libraries(
    chat_security
    PUBLIC OpenSSL::Crypto
)

add_library(chat_database
    src/database_config.cpp
    src/mysql_user_repository.cpp
)

target_include_directories(
    chat_database
    PUBLIC include
)

target_link_libraries(
    chat_database
    PUBLIC
        chat_common
        chat_security
        PkgConfig::MYSQLCLIENT
)

add_library(chat_server_core
    src/chat_server.cpp
)

target_include_directories(
    chat_server_core
    PUBLIC include
)

target_link_libraries(
    chat_server_core
    PUBLIC chat_common
)

add_executable(chat_server
    src/server_main.cpp
)

target_link_libraries(
    chat_server
    PRIVATE
        chat_server_core
        chat_database
)

add_executable(chat_client
    src/client.cpp
)

target_link_libraries(
    chat_client
    PRIVATE chat_common
)

foreach(
    target
    chat_common
    chat_security
    chat_database
    chat_server_core
    chat_server
    chat_client
)
    target_compile_options(
        ${target}
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endforeach()

if(BUILD_TESTING)
    add_executable(
        test_password_hasher
        tests/test_password_hasher.cpp
    )

    target_link_libraries(
        test_password_hasher
        PRIVATE chat_security
    )

    target_compile_options(
        test_password_hasher
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    add_test(
        NAME password_hasher
        COMMAND test_password_hasher
    )

    add_executable(
        chat_server_test
        tests/test_server_main.cpp
        tests/in_memory_user_repository.cpp
    )

    target_include_directories(
        chat_server_test
        PRIVATE
            include
            tests
    )

    target_link_libraries(
        chat_server_test
        PRIVATE chat_server_core
    )

    target_compile_options(
        chat_server_test
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()
