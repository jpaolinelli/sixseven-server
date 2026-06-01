# ============================================================================
# SixSevenDB — Multi-stage Docker build
#
# Stages:
#   1. build    — install tools + vcpkg deps + compile release binaries
#   2. test     — build debug + run unit/QA tests (optional target)
#   3. runtime  — slim image with server binary + ONNX model
#
# Build:
#   docker build -t sixsevendb .                            # runtime image
#   docker build --target test -t sixsevendb-test .         # build + test
#
# Run:
#   docker run -p 5432:5432 sixsevendb
# ============================================================================

# ---------------------------------------------------------------------------
# Stage 1: build
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

# Build tools + GCC 13 (Clang has known issues with onnxruntime intrinsics)
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-13 g++-13 cmake ninja-build make git curl zip unzip tar pkg-config \
        ca-certificates python3 python3-dev python3-pip \
        linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

# Install vcpkg (pinned to the same commit as vcpkg.json builtin-baseline)
ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} \
    && cd ${VCPKG_ROOT} \
    && git checkout 1940ee77e81573713c0d364c42f5990172198be1 \
    && ./bootstrap-vcpkg.sh -disableMetrics

# Create a release-only triplet to halve vcpkg build time
RUN printf 'set(VCPKG_TARGET_ARCHITECTURE x64)\nset(VCPKG_CRT_LINKAGE dynamic)\nset(VCPKG_LIBRARY_LINKAGE dynamic)\nset(VCPKG_CMAKE_SYSTEM_NAME Linux)\nset(VCPKG_BUILD_TYPE release)\n' \
    > ${VCPKG_ROOT}/triplets/community/x64-linux-release.cmake

# Copy full source
WORKDIR /src
COPY . .

# Configure + build release with GCC 13
ENV CC=gcc-13 CXX=g++-13
RUN cmake --preset release \
        -DVCPKG_TARGET_TRIPLET=x64-linux-release \
        -DVCPKG_HOST_TRIPLET=x64-linux-release \
    && cmake --build build/release -j$(nproc)

# Download the ONNX embedding model for the runtime image
RUN pip install --break-system-packages huggingface-hub \
    && python3 -c "from huggingface_hub import snapshot_download; snapshot_download('onnx-community/all-MiniLM-L6-v2-ONNX', local_dir='/opt/models/all-MiniLM-L6-v2')" \
    && rm -rf /opt/models/all-MiniLM-L6-v2/.cache

# ---------------------------------------------------------------------------
# Stage 2: test (optional — use: docker build --target test)
# ---------------------------------------------------------------------------
FROM build AS test

RUN cmake --preset default \
        -DVCPKG_TARGET_TRIPLET=x64-linux-release \
        -DVCPKG_HOST_TRIPLET=x64-linux-release \
    && cmake --build build/debug -j$(nproc)

RUN ctest --test-dir build/debug -L unit --output-on-failure
RUN ctest --test-dir build/debug -L qa --output-on-failure

# ---------------------------------------------------------------------------
# Stage 3: slim runtime image
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 libgcc-s1 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /bin/false sixseven \
    && mkdir -p /data /config /models \
    && chown -R sixseven:sixseven /data /config /models

# Copy the release binaries
COPY --from=build /src/build/release/src/sixseven-server /usr/local/bin/
COPY --from=build /src/build/release/src/sixseven-cli /usr/local/bin/

# Copy shared libraries the binaries need
COPY --from=build /src/build/release/vcpkg_installed/x64-linux-release/lib/*.so* /usr/local/lib/
RUN ldconfig

# Bundle the ONNX embedding model so vector search works out of the box
COPY --from=build /opt/models/all-MiniLM-L6-v2 /models/all-MiniLM-L6-v2

EXPOSE 5432
VOLUME ["/data", "/config"]
USER sixseven

ENTRYPOINT ["sixseven-server"]
CMD ["/config/config.json"]
