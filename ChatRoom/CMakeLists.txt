cmake_minimum_required(VERSION 3.16)

project(chatroom_v6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(chat_common
    src/protocol.cpp
    src/message_store.cpp
)

target_include_directories(chat_common PUBLIC include)

add_executable(chat_server
    src/server_main.cpp
    src/chat_server.cpp
)

target_include_directories(chat_server PRIVATE include)
target_link_libraries(chat_server PRIVATE chat_common)

add_executable(chat_client
    src/client.cpp
)

target_include_directories(chat_client PRIVATE include)
target_link_libraries(chat_client PRIVATE chat_common)

foreach(target chat_common chat_server chat_client)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endforeach()
