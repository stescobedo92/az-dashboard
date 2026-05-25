FROM ubuntu:24.04@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b AS build

ARG VCPKG_ROOT=/opt/vcpkg
ARG VCPKG_COMMIT=aa40adda5352e87655b8583cfb2451d5e9e276fd
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
    && git -C "${VCPKG_ROOT}" checkout "${VCPKG_COMMIT}" \
    && "${VCPKG_ROOT}/bootstrap-vcpkg.sh"

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
      -DAZ_DASHBOARD_BUILD_TESTS=ON \
    && cmake --build build \
    && ctest --test-dir build --output-on-failure

FROM mcr.microsoft.com/azure-cli:2.73.0@sha256:eedd1fb2d6628c6a6f30c5e0cd31b2fd789151514fa0ab645ff440682e5586b7

RUN mkdir -p /home/azdash \
    && chown 10001:10001 /home/azdash
ENV HOME=/home/azdash

COPY --from=build /src/build/azdash /usr/local/bin/azdash

USER 10001:10001

ENTRYPOINT ["azdash"]
