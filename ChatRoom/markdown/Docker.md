FROM ubuntu:24.04


ENV DEBIAN_FRONTEND=noninteractive


RUN apt update && apt install -y \
    build-essential \
    cmake \
    pkg-config \
    protobuf-compiler \
    libprotobuf-dev \
    libssl-dev \
    libmysqlclient-dev \
    libsqlite3-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*


WORKDIR /app


COPY . .


RUN mkdir -p build && \
    cd build && \
    cmake .. && \
    make -j$(nproc)


CMD ["./build/bin/server_main"]