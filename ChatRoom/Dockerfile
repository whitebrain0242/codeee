FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

# 安装编译依赖
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libmysqlclient-dev \
    libhiredis-dev \
    libsqlite3-dev \
    libssl-dev \
    protobuf-compiler \
    libprotobuf-dev \
    libspdlog-dev \
    libfmt-dev \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 拷贝全部源码
COPY CMakeLists.txt .
COPY proto/ proto/
COPY src/ src/
COPY include/ include/
COPY tests/ tests/

# ========== 自动修复源码 ==========
# 1. 修复 mysql_database.cpp 中的 std::vector<bool> 取地址问题
RUN sed -i 's/std::vector<bool>/std::vector<unsigned char>/g' src/mysql_database.cpp

# 2. 修复客户端缺失的头文件（解决 ClientAppConfig 等未声明错误）
RUN sed -i '1i#include "client/client_app.h"\n#include "client/client_common.h"' src/client.cpp && \
    sed -i '1i#include "client/client_app.h"\n#include "client/client_common.h"' src/client/client_app.cpp

# ========== 编译整个项目 ==========
RUN mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc)

EXPOSE 8000 8443
CMD ["./build/chat_server"]