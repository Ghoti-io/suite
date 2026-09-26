# Toolchain for the public manual. The tree is mounted at /work; this image
# does not copy it. docs.sh --container builds the image and runs it.
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      doxygen \
      graphviz \
      cloc \
      python3 \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
ENTRYPOINT ["/work/suite/docs.sh"]
