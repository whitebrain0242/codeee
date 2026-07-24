cmake_minimum_required(VERSION 3.16)

project(chatroom_v5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(chat_common
    src/protocol.cpp
)

target_include_directories(chat_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(chat_common
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_executable(chat_server
    src/server_main.cpp
    src/chat_server.cpp
)

target_include_directories(chat_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chat_server
    PRIVATE
        chat_common
)

target_compile_options(chat_server
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_executable(chat_client
    src/client.cpp
)

target_include_directories(chat_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chat_client
    PRIVATE
        chat_common
)

target_compile_options(chat_client
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)
