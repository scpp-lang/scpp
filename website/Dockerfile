FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive \
    CC=clang-22 \
    CXX=clang++-22 \
    LLVM_CMAKE_DIR=/usr/lib/llvm-22/lib/cmake/llvm

WORKDIR /work

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl gnupg cmake ninja-build build-essential libzstd-dev zlib1g-dev pandoc; \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm-archive-keyring.gpg; \
    echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-22 main" > /etc/apt/sources.list.d/llvm.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends clang-22 llvm-22-dev; \
    rm -rf /var/lib/apt/lists/*

COPY . .

RUN cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="${LLVM_CMAKE_DIR}" && \
    cmake --build build -j2

ARG DOCS_BUILD_SCRIPT=docs/site/build.sh
ARG DOCS_BUILD_OUTPUT_DIR=docs/site/dist
RUN SITE_OUTPUT_DIR=/work/site-dist \
    DOCS_BUILD_SCRIPT="${DOCS_BUILD_SCRIPT}" \
    DOCS_BUILD_OUTPUT_DIR="${DOCS_BUILD_OUTPUT_DIR}" \
    bash website/build_site_for_docker.sh

FROM ubuntu:24.04 AS runtime

RUN set -eux; \
    groupadd --system scpp; \
    useradd --system --gid scpp --home-dir /srv/scpp-site --create-home scpp

WORKDIR /srv/scpp-site
COPY --from=build /work/build/applications/httpserver/httpserver_app_bin ./scpp-httpserver
COPY --from=build /work/site-dist/ ./
RUN chown -R scpp:scpp /srv/scpp-site && chmod +x ./scpp-httpserver

USER scpp
EXPOSE 8080
CMD ["./scpp-httpserver"]
