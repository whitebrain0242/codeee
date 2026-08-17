cmake_minimum_required(VERSION 3.10)
project(EchoServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 OpenSSL（TlsContext 需要）
find_package(OpenSSL REQUIRED)

# 包含头文件路径（项目根目录为起始）
include_directories("${CMAKE_SOURCE_DIR}")

# 收集所有 minimuduo/net/*.cpp 源文件
file(GLOB_RECURSE LIB_SOURCES "minimuduo/net/*.cpp")

# 生成可执行文件
add_executable(echo_server main.cpp ${LIB_SOURCES})

# 链接 OpenSSL 和 pthread
target_link_libraries(echo_server
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

# （可选）如果编译时提示某些宏未定义，可加：
# target_compile_definitions(echo_server PRIVATE _GNU_SOURCE)