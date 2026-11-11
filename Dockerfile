# MiniVectorDB -- 2026 OceanBase competition, round 2.
#
# Only the standalone database in minivectordb/ is built; the surrounding
# seekdb tree is irrelevant to this task and is deliberately not compiled.
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make \
 && rm -rf /var/lib/apt/lists/*

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
