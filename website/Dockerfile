FROM ubuntu:26.04 AS build

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    CC=clang-22 \
    CXX=clang++-22 \
    LLVM_CMAKE_DIR=/usr/lib/llvm-22/lib/cmake/llvm

WORKDIR /work

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl cmake ninja-build build-essential libzstd-dev libsqlite3-dev zlib1g-dev pandoc clang-22 llvm-22-dev; \
    rm -rf /var/lib/apt/lists/*

COPY . .

RUN cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH="${LLVM_CMAKE_DIR}" \
    -DSCPP_HTTPSERVER_BUILD_STATIC=ON && \
    cmake --build build -j2

ARG DOCS_BUILD_SCRIPT=docs/site/build.sh
ARG DOCS_BUILD_OUTPUT_DIR=docs/site/dist
RUN SITE_OUTPUT_DIR=/work/site-dist \
    DOCS_BUILD_SCRIPT="${DOCS_BUILD_SCRIPT}" \
    DOCS_BUILD_OUTPUT_DIR="${DOCS_BUILD_OUTPUT_DIR}" \
    bash website/build_site_for_docker.sh

FROM scratch AS runtime

WORKDIR /srv/scpp-site
COPY --from=build --chown=65532:65532 /work/build/applications/httpserver/httpserver_app_bin /scpp-httpserver
COPY --from=build --chown=65532:65532 /work/site-dist/ /srv/scpp-site/

USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/scpp-httpserver"]
