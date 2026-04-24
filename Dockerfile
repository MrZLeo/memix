FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    ca-certificates \
    ccache \
    clang \
    clang-format \
    clang-tidy \
    cmake \
    cppcheck \
    curl \
    doxygen \
    g++-14 \
    gcc-14 \
    git \
    graphviz \
    make \
    ninja-build \
    python3 \
    tar \
    unzip \
    vim \
    wget \
    && rm -rf /var/lib/apt/lists/*

ENV CC=gcc-14
ENV CXX=g++-14
