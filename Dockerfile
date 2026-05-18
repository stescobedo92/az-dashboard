FROM ubuntu:24.04 AS build

ARG VCPKG_ROOT=/opt/vcpkg
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      curl \
      git \
      ninja-build \
      pkg-config \
      tar \
      unzip \
      zip \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" \
    && "${VCPKG_ROOT}/bootstrap-vcpkg.sh"

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
      -DAZ_DASHBOARD_BUILD_TESTS=ON \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure

FROM mcr.microsoft.com/azure-cli:2.73.0

COPY --from=build /src/build/azdash /usr/local/bin/azdash

ENTRYPOINT ["azdash"]
