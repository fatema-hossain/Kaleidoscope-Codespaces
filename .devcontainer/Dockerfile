FROM mcr.microsoft.com/vscode/devcontainers/cpp:ubuntu-24.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       clang \
       lldb \
       lld \
       llvm-dev \
       llvm-runtime \
       cmake \
       ninja-build \
       git \
       python3 \
    && rm -rf /var/lib/apt/lists/*
