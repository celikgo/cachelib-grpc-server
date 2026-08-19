# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Multi-stage Dockerfile for cachelib-grpc-server.
# Upstream CacheLib is cloned as a build dependency, never vendored.
# Stage 1: Build environment
FROM ubuntu:24.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install base dependencies for getdeps.py and gRPC
RUN apt-get update && apt-get install -y \
    autoconf \
    automake \
    binutils-dev \
    bison \
    build-essential \
    cmake \
    curl \
    flex \
    git \
    libboost-all-dev \
    libdouble-conversion-dev \
    libevent-dev \
    libgflags-dev \
    libgmock-dev \
    libgoogle-glog-dev \
    libgtest-dev \
    libiberty-dev \
    libjemalloc-dev \
    liblz4-dev \
    liblzma-dev \
    libnuma-dev \
    libsnappy-dev \
    libsodium-dev \
    libssl-dev \
    libtool \
    libunwind-dev \
    libzstd-dev \
    libaio-dev \
    libfmt-dev \
    ninja-build \
    pkg-config \
    python3 \
    python3-pip \
    wget \
    zlib1g-dev \
    sudo \
    libxxhash-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Install gRPC and protobuf from source for consistent versions
ARG GRPC_VERSION=v1.60.0
RUN git clone --recurse-submodules -b ${GRPC_VERSION} --depth 1 https://github.com/grpc/grpc && \
    cd grpc && \
    mkdir -p cmake/build && \
    cd cmake/build && \
    cmake -DgRPC_INSTALL=ON \
          -DgRPC_BUILD_TESTS=OFF \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -G Ninja \
          ../.. && \
    ninja -j$(nproc) && \
    ninja install && \
    cd /build && \
    rm -rf grpc

# Download grpc_health_probe binary for container health checks
RUN GRPC_HEALTH_PROBE_VERSION=v0.4.25 && \
    ARCH=$(dpkg --print-architecture) && \
    wget -qO/usr/local/bin/grpc_health_probe \
      "https://github.com/grpc-ecosystem/grpc-health-probe/releases/download/${GRPC_HEALTH_PROBE_VERSION}/grpc_health_probe-linux-${ARCH}" && \
    chmod +x /usr/local/bin/grpc_health_probe

# NOTE: NOT installing liburing - folly's io_uring support requires
# kernel features not available in Docker VM. By not having liburing,
# folly will build without io_uring support.

# Install magic_enum via cmake (creates config files for find_package)
RUN git clone --depth 1 --branch v0.9.5 https://github.com/Neargye/magic_enum.git && \
    cd magic_enum && \
    mkdir build && cd build && \
    cmake -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF \
          -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF \
          .. && \
    make install && \
    cd /build && \
    rm -rf magic_enum

# Clone CacheLib from GitHub (ensures all dependencies are properly fetched)
ARG CACHELIB_REPO=https://github.com/facebook/CacheLib.git
# Pinned to a commit, not a branch. Two reasons:
#
#  1. Reproducibility. Cloning `main` means rebuilding a release tag pulls
#     whatever upstream happens to be that day, so the build would not
#     reproduce the release.
#  2. Buildability. This is upstream main as of 2026-05-02, the revision the
#     last known-good published image (1.6.0) was built from. Later upstream
#     revisions bump mvfst to a version that does not compile under GCC 13 on
#     Ubuntu 24.04 (`default member initializer ... required before the end of
#     its enclosing class` in quic::DatagramFlowManager), which fails the build
#     roughly 30 minutes in.
#
# Bumping this is a deliberate act: re-cut patches/ against the new revision
# (they will stop applying, by design) and confirm mvfst still compiles.
ARG CACHELIB_REF=2aa2afbe97deadfb00f15260b26b566354c57a78
RUN git clone ${CACHELIB_REPO} CacheLib && \
    cd CacheLib && git checkout --detach ${CACHELIB_REF}

# Copy this repository (the gRPC server) into the upstream tree as a subproject.
COPY CMakeLists.txt build.sh /build/CacheLib/standalone_server/
COPY proto /build/CacheLib/standalone_server/proto
COPY cmake /build/CacheLib/standalone_server/cmake
COPY tests /build/CacheLib/standalone_server/tests
COPY *.cc *.h /build/CacheLib/standalone_server/

# Apply our patches to the pinned upstream tree. These are real diffs, so if a
# CACHELIB_REF bump makes one stop applying, the build fails here immediately
# and loudly rather than much later with a confusing error.
# See patches/README.md for what each one changes and why.
COPY patches/*.patch /tmp/patches/
RUN cd /build/CacheLib && git apply --verbose /tmp/patches/*.patch

# Build CacheLib dependencies only (not cachelib itself) using getdeps.py
WORKDIR /build/CacheLib
RUN python3 ./build/fbcode_builder/getdeps.py --allow-system-packages build --only-deps cachelib

# Get the install prefix used by getdeps
RUN INSTALL_PREFIX=$(python3 ./build/fbcode_builder/getdeps.py --allow-system-packages show-inst-dir fbthrift | xargs dirname) && \
    echo "INSTALL_PREFIX=${INSTALL_PREFIX}" && \
    ln -s ${INSTALL_PREFIX} /opt/getdeps-install

# Build CacheLib manually with patched source
WORKDIR /build/CacheLib
RUN mkdir -p cachelib-build && \
    cd cachelib-build && \
    INSTALL_PREFIX=$(readlink -f /opt/getdeps-install) && \
    # Build CMAKE_PREFIX_PATH from all installed dependencies
    CMAKE_PATHS="" && \
    for dir in ${INSTALL_PREFIX}/*/; do \
        if [ -z "$CMAKE_PATHS" ]; then \
            CMAKE_PATHS="${dir}"; \
        else \
            CMAKE_PATHS="${CMAKE_PATHS};${dir}"; \
        fi; \
    done && \
    CMAKE_PATHS="${CMAKE_PATHS};/usr/local" && \
    echo "CMAKE_PREFIX_PATH=${CMAKE_PATHS}" && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH="${CMAKE_PATHS}" \
          -DCMAKE_INSTALL_PREFIX=/opt/cachelib \
          -DBUILD_TESTS=OFF \
          -G Ninja \
          ../cachelib && \
    ninja -j$(nproc) cachelib_allocator cachelib_navy cachelib_common cachelib_shm cachelib_datatype cachelib_nvmitem && \
    mkdir -p /opt/cachelib/lib /opt/cachelib/include/cachelib && \
    cp allocator/libcachelib_allocator.a /opt/cachelib/lib/ && \
    cp allocator/libcachelib_nvmitem.a /opt/cachelib/lib/ && \
    cp navy/libcachelib_navy.a /opt/cachelib/lib/ && \
    cp common/libcachelib_common.a /opt/cachelib/lib/ && \
    cp shm/libcachelib_shm.a /opt/cachelib/lib/ && \
    cp datatype/libcachelib_datatype.a /opt/cachelib/lib/ && \
    cp -r ../cachelib/* /opt/cachelib/include/cachelib/ && \
    cp -r cachelib/* /opt/cachelib/include/cachelib/

# Create symlink for compatibility
RUN ln -s /opt/cachelib /opt/cachelib-install

# Copy magic_enum headers alongside CacheLib headers (needed by EventSink.h)
RUN cp -r /opt/getdeps-install/magic_enum-*/include/magic_enum /opt/cachelib/include/magic_enum && \
    ls /opt/cachelib/include/magic_enum/magic_enum.hpp

# Build the gRPC server
WORKDIR /build/CacheLib/standalone_server
RUN mkdir -p build && \
    cd build && \
    INSTALL_PREFIX=$(readlink -f /opt/getdeps-install) && \
    # Build CMAKE_PREFIX_PATH from all installed dependencies
    CMAKE_PATHS="/opt/cachelib" && \
    for dir in ${INSTALL_PREFIX}/*/; do CMAKE_PATHS="${CMAKE_PATHS};${dir}"; done && \
    CMAKE_PATHS="${CMAKE_PATHS};/usr/local" && \
    echo "CMAKE_PREFIX_PATH=${CMAKE_PATHS}" && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH="${CMAKE_PATHS}" \
          -DCMAKE_EXE_LINKER_FLAGS="-Wl,--copy-dt-needed-entries" \
          -DBUILD_TESTS=OFF \
          -G Ninja \
          .. && \
    ninja -j$(nproc)

# Stage 2: Test stage (optional, use --target tester)
FROM builder AS tester
WORKDIR /build/CacheLib/standalone_server
RUN cd build && \
    cmake -DBUILD_TESTS=ON . && \
    ninja -j$(nproc) cache_manager_test && \
    ./cache_manager_test

# Stage 3: Runtime image
FROM ubuntu:24.04 AS runtime

# OCI labels. org.opencontainers.image.source is what links the published
# package to this repository on GHCR.
LABEL org.opencontainers.image.source="https://github.com/celikgo/cachelib-grpc-server" \
      org.opencontainers.image.description="A standalone gRPC server for Meta's CacheLib: 19 RPCs over a Redis-style key/value surface, on a hybrid DRAM+SSD cache." \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.title="cachelib-grpc-server" \
      org.opencontainers.image.documentation="https://github.com/celikgo/cachelib-grpc-server/blob/main/README.md"

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies only (Ubuntu 24.04 packages)
RUN apt-get update && apt-get install -y \
    libaio1t64 \
    libboost-context1.83.0 \
    libboost-filesystem1.83.0 \
    libboost-iostreams1.83.0 \
    libboost-program-options1.83.0 \
    libboost-regex1.83.0 \
    libboost-system1.83.0 \
    libboost-thread1.83.0 \
    libdouble-conversion3 \
    libevent-2.1-7t64 \
    libgflags2.2 \
    libgoogle-glog0v6t64 \
    libjemalloc2 \
    liblz4-1 \
    libnuma1 \
    libsnappy1v5 \
    libsodium23 \
    libssl3t64 \
    libunwind8 \
    libzstd1 \
    liburing2 \
    libxxhash0 \
    && rm -rf /var/lib/apt/lists/*

# Create a non-root user for security
RUN useradd -m -s /bin/bash cachelib

# Copy built artifacts from builder stage
COPY --from=builder /opt/cachelib/lib /opt/cachelib/lib
COPY --from=builder /build/CacheLib/standalone_server/build/cachelib-grpc-server /usr/local/bin/
COPY --from=builder /usr/local/lib/libgrpc*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libprotobuf*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libabsl*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libre2*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libupb*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libaddress_sorting*.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libgpr*.so* /usr/local/lib/

# Copy getdeps-built shared libraries (folly, glog, fbthrift, etc.)
COPY --from=builder /opt/getdeps-install/folly/lib/*.so* /usr/local/lib/
COPY --from=builder /opt/getdeps-install/glog-*/lib/*.so* /usr/local/lib/
COPY --from=builder /opt/getdeps-install/fmt-*/lib/*.so* /usr/local/lib/
COPY --from=builder /opt/getdeps-install/fbthrift/lib/*.so* /usr/local/lib/
COPY --from=builder /opt/getdeps-install/fizz/lib/*.so* /usr/local/lib/
COPY --from=builder /opt/getdeps-install/wangle/lib/*.so* /usr/local/lib/
COPY --from=builder /opt/getdeps-install/mvfst/lib/*.so* /usr/local/lib/
COPY --from=builder /opt/getdeps-install/liboqs-*/lib/*.so* /usr/local/lib/

# Copy grpc_health_probe for health checks
COPY --from=builder /usr/local/bin/grpc_health_probe /usr/local/bin/

# Copy proto file for reference
COPY --from=builder /build/CacheLib/standalone_server/proto/cache.proto /opt/cachelib/proto/

# Update library cache
RUN ldconfig

# Create directories for NVM cache (flash storage)
RUN mkdir -p /data/nvm && chown cachelib:cachelib /data/nvm

# Switch to non-root user
USER cachelib

# Set library path
ENV LD_LIBRARY_PATH="/opt/cachelib/lib:/usr/local/lib"

# Expose gRPC port and Prometheus metrics port
EXPOSE 50051
EXPOSE 9090

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD grpc_health_probe -addr=:50051 || exit 1

# Default command with sensible defaults
ENTRYPOINT ["/usr/local/bin/cachelib-grpc-server"]
CMD ["--address=0.0.0.0", "--port=50051", "--cache_size=1073741824"]
