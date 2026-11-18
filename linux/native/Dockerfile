# Build environment for the native Linux port (Ubuntu 24.04, x86_64 or aarch64).
# It contains only public toolchains and libraries; the game is never inside.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang cmake ninja-build pkg-config python3 git ca-certificates unzip \
        libsdl2-dev libvulkan-dev glslang-tools libpng-dev \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
