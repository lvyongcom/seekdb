# MiniVectorDB -- 2026 OceanBase competition, round 2.
#
# Only the standalone database in minivectordb/ is built; the surrounding
# seekdb tree is irrelevant to this task and is deliberately not compiled.
FROM ubuntu:24.04 AS build

# The apt step retries through a mirror if the default archive is unreachable.
# A builder that cannot reach archive.ubuntu.com would otherwise fail the whole
# submission at image-build time, which is an environment difference rather
# than a defect in the database.
RUN set -eux; \
    install_toolchain() { \
        apt-get update; \
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            g++ make; \
    }; \
    if ! install_toolchain; then \
        for file in /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list; do \
            [ -f "$file" ] || continue; \
            sed -i \
                -e 's|http://archive.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g' \
                -e 's|http://security.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g' \
                "$file"; \
        done; \
        install_toolchain; \
    fi; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY minivectordb/ /src/

# Static link: the runtime stage then needs no C++ toolchain, no shared
# libstdc++, and no dynamic-loader surprises.
RUN make -j"$(nproc)" LDFLAGS="-static"


FROM ubuntu:24.04

# The judge runs this image with a read-only root filesystem and a volume
# mounted at /data, so /data is the only writable location at runtime.
RUN mkdir -p /data
VOLUME ["/data"]

COPY --from=build /src/build/minivectordb /usr/local/bin/minivectordb

ENTRYPOINT ["/usr/local/bin/minivectordb"]
CMD ["--data-dir", "/data"]
