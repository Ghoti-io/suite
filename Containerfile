# Toolchain for the public manual. The tree is mounted at /work; this image
# does not copy it. Tag ghoti-docs:doxygen-1.9.8. A bare docs.sh builds this
# image and runs it.
#
# Debian 13, digest-pinned, so Doxygen is 1.9.8. The manual's page ids use
# that version's names for a markdown file in a subdirectory. Debian 12's
# Doxygen 1.9.4 names the same file differently, and the menu then cannot
# be assembled. Packages are pinned to exact versions: a later rebuild that
# cannot resolve one fails.
FROM docker.io/library/debian:trixie-slim@sha256:918311b7b6c4c6f68b232ba516584925f6c78ad82b6fd534b98979df6438e483

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      doxygen=1.9.8+ds-2.1 \
      graphviz=2.42.4-3 \
      cloc=2.04-1 \
      python3=3.13.5-1 \
      ca-certificates=20250419 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
ENTRYPOINT ["/work/suite/docs.sh"]
