#!/bin/bash
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

# Build script for CacheLib gRPC Server

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
CACHELIB_PREFIX="${CACHELIB_PREFIX:-/opt/cachelib}"
BUILD_TYPE="Release"
BUILD_TESTS="OFF"
PARALLEL_JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -d, --debug       Build in Debug mode (default: Release)"
    echo "  -t, --tests       Build tests"
    echo "  -j, --jobs N      Number of parallel jobs (default: auto-detect)"
    echo "  -c, --clean       Clean build directory before building"
    echo "  -h, --help        Show this help message"
    echo ""
    echo "Prerequisites:"
    echo "  - Linux with CacheLib headers/libraries installed at CACHELIB_PREFIX"
    echo "    (default: /opt/cachelib); build the pinned dependency first"
    echo "  - gRPC, Protobuf, folly and CacheLib's other dependencies installed"
    echo "  - Set CMAKE_PREFIX_PATH to additional dependency prefixes"
    echo "  - For a complete reproducible environment, use the Dockerfile"
    echo ""
    exit 0
}

log() {
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*"
}

error() {
    echo "[ERROR] $*" >&2
    exit 1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -t|--tests)
            BUILD_TESTS="ON"
            shift
            ;;
        -j|--jobs)
            [[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || error "--jobs requires a positive integer"
            PARALLEL_JOBS="$2"
            shift 2
            ;;
        -c|--clean)
            rm -rf "${BUILD_DIR}"
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            error "Unknown option: $1"
            ;;
    esac
done

log "=== CacheLib gRPC Server Build ==="
log "Build type: ${BUILD_TYPE}"
log "Build tests: ${BUILD_TESTS}"
log "Parallel jobs: ${PARALLEL_JOBS}"

# Check if CacheLib is built
if [[ ! -d "${CACHELIB_PREFIX}" ]]; then
    error "CacheLib not found at ${CACHELIB_PREFIX}. Set CACHELIB_PREFIX to its installed prefix, or build with Docker (see README.md)."
fi

# Check for gRPC
if ! command -v grpc_cpp_plugin &>/dev/null; then
    log "Warning: grpc_cpp_plugin not found in PATH. CMake will try to locate it."
fi

# Create build directory
mkdir -p "${BUILD_DIR}"

log "Configuring with CMake..."

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCACHELIB_ROOT="${CACHELIB_PREFIX}" \
    -DCMAKE_PREFIX_PATH="${CACHELIB_PREFIX};${CMAKE_PREFIX_PATH:-/usr/local}" \
    -DBUILD_TESTS="${BUILD_TESTS}"

log "Building..."

cmake --build "${BUILD_DIR}" --parallel "${PARALLEL_JOBS}"

log "Build completed successfully!"
log ""
log "Binary location: ${BUILD_DIR}/cachelib-grpc-server"
log ""
log "To run the server:"
log "  ${BUILD_DIR}/cachelib-grpc-server --port=50051 --cache_size=1073741824"
log ""

if [[ "${BUILD_TESTS}" == "ON" ]]; then
    log "To run tests:"
    log "  cd ${BUILD_DIR} && ctest --output-on-failure"
fi
