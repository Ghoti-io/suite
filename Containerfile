# Toolchain for the public manual. The tree is mounted at /work; this image
# does not copy it. docs.sh --container builds the image and runs it.
#
# Debian 13, so Doxygen is 1.9.8. The manual's page ids use that version's
# names for a markdown file in a subdirectory. Debian 12's Doxygen 1.9.4
# names the same file differently, and the menu then cannot be assembled.
FROM debian:trixie-slim

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
