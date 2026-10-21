# The shape benchmark's own programs: bench-shape (the scene generator, and
# shape-cpu and font measured) and pamdiff (the cross-check's measure).
#
#   make PREFIX=/path/to/prefix [BUILD_DIR=build]
#
# Built against an installed suite through pkg-config only, like every library
# here. The reference drivers (refs/) are not built here: they are compiled
# inside their own pinned images, by run.sh.
#
# Release flags: -O2, as the libraries ship (their own Makefiles decide their
# objects' flags; these are only the harness's).

ifeq ($(PREFIX),)
$(error PREFIX= names the prefix holding shape-cpu, shape-scene, shape-geometry, image and font)
endif

BUILD_DIR ?= build
CC ?= gcc
PKGS := ghoti.io-shape-cpu-0 ghoti.io-shape-scene-0 ghoti.io-shape-geometry-0 ghoti.io-image-0 ghoti.io-font-0
PC := PKG_CONFIG_PATH="$(PREFIX)/share/pkgconfig$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))" pkg-config
PKG_CFLAGS := $(shell $(PC) --cflags $(PKGS))
PKG_LIBS := $(shell $(PC) --libs $(PKGS))
comma := ,
RPATH := $(patsubst -L%,-Wl$(comma)-rpath$(comma)%,$(filter -L%,$(PKG_LIBS)))

ifeq ($(PKG_LIBS),)
$(error pkg-config cannot find $(PKGS) under $(PREFIX))
endif

BENCH_CFLAGS := -std=c17 -O2 -g -Wall -Wextra -Werror -ffp-contract=off

all: $(BUILD_DIR)/bench-shape $(BUILD_DIR)/pamdiff

$(BUILD_DIR)/bench-shape: bench-shape.c scene.c scene.h Makefile
	@mkdir -p $(@D)
	$(CC) $(BENCH_CFLAGS) $(PKG_CFLAGS) -o $@ bench-shape.c scene.c $(PKG_LIBS) $(RPATH) -lm

$(BUILD_DIR)/pamdiff: pamdiff.c Makefile
	@mkdir -p $(@D)
	$(CC) $(BENCH_CFLAGS) -o $@ pamdiff.c

# The flags, as a recorded fact for the results.
flags:
	@echo '$(CC) $(BENCH_CFLAGS)'

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean flags
