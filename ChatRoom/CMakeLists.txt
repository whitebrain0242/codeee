cmake_minimum_required(VERSION 3.16)

project(chatroom_v7_2 LANGUAGES CXX)

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

option(
    CHATROOM_USE_SYSTEM_PROTOBUF
    "Use official Protobuf generated C++ classes when available"
    ON
)

if(CHATROOM_USE_SYSTEM_PROTOBUF)
    find_package(Protobuf QUIET)
endif()

if(
    Protobuf_FOUND AND
    Protobuf_PROTOC_EXECUTABLE
)
    protobuf_generate_cpp(
        FRIEND_EVENT_PROTO_SRCS
        FRIEND_EVENT_PROTO_HDRS
        proto/friend_event.proto
    )

    add_library(chat_serialization
        src/friend_event_codec_official.cpp
        ${FRIEND_EVENT_PROTO_SRCS}
        ${FRIEND_EVENT_PROTO_HDRS}
    )

    target_include_directories(
        chat_serialization
        PUBLIC
            include
            ${CMAKE_CURRENT_BINARY_DIR}
    )

    target_link_libraries(
        chat_serialization
        PUBLIC protobuf::libprotobuf
    )

    target_compile_definitions(
        chat_serialization
        PUBLIC CHATROOM_OFFICIAL_PROTOBUF=1
    )

    message(
        STATUS
        "Using official Protobuf generated C++ classes"
    )
else()
    add_library(chat_serialization
        src/friend_event_codec.cpp
    )

    target_include_directories(
        chat_serialization
        PUBLIC include
    )

    target_compile_definitions(
        chat_serialization
        PUBLIC CHATROOM_BUNDLED_PROTOBUF_CODEC=1
    )

    message(
        WARNING
        "Official Protobuf development tools were not found; "
        "using the bundled proto3-compatible codec. "
        "Install protobuf-compiler and libprotobuf-dev "
        "to use protoc-generated classes."
    )
endif()

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
    src/mysql_friend_repository.cpp
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
    PUBLIC
        chat_common
        chat_serialization
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
    chat_serialization
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
        test_friend_event_codec
        tests/test_friend_event_codec.cpp
    )

    target_link_libraries(
        test_friend_event_codec
        PRIVATE chat_serialization
    )

    target_compile_options(
        test_friend_event_codec
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )

    add_test(
        NAME friend_event_codec
        COMMAND test_friend_event_codec
    )

    add_executable(
        chat_server_test
        tests/test_server_main.cpp
        tests/in_memory_user_repository.cpp
        tests/in_memory_friend_repository.cpp
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
