# Stage 1: Build
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Use Chinese mirror for faster & reliable access
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.ustc.edu.cn|g; s|http://security.ubuntu.com|http://mirrors.ustc.edu.cn|g' /etc/apt/sources.list

# Build dependencies (libsdl2/libgl needed by CMakeLists.txt's find_package)
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    gcc-11 \
    g++-11 \
    make \
    libhiredis-dev \
    libmysqlclient-dev \
    libssl-dev \
    libsdl2-dev \
    libgl-dev \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /project

# Header-only dependencies
COPY third/    third/
COPY include/  include/

# Source code + build config
COPY chatting_room/  chatting_room/
COPY example/       example/
COPY CMakeLists.txt  .

# Release build (only server_node + sqlite_service for the full distributed system)
RUN cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc-11 -DCMAKE_CXX_COMPILER=g++-11 \
    && cmake --build . --parallel $(nproc) --target server_node sqlite_service

# Stage 2: Runtime
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Use Chinese mirror
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.ustc.edu.cn|g; s|http://security.ubuntu.com|http://mirrors.ustc.edu.cn|g' /etc/apt/sources.list

# Runtime libraries only
RUN apt-get update && apt-get install -y --no-install-recommends \
    libhiredis0.14 \
    libmysqlclient21 \
    libssl3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /project/bin/server_node     /app/server_node
COPY --from=builder /project/bin/sqlite_service  /app/sqlite_service

# 创建日志目录并赋予 nobody 写权限
RUN mkdir -p /app/logs && chown nobody:nogroup /app/logs

WORKDIR /app

# Use unprivileged user
# USER nobody

ENTRYPOINT ["/app/server_node"]
