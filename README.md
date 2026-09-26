# Ghoti.io

Scripts for the [Ghoti.io](https://github.com/Ghoti-io) libraries: clone them,
build and install them, and build the combined manual.

The libraries are separate repositories. This one is how you get the set.
Someone who wants a single library clones that repository on its own.

Checked out, the tree looks like this. `libs/`, `docs/` and `.local/` are
siblings of this directory, and this repository does not contain them.

```
<parent>/
  suite/     this repository
  libs/      cloned by ./clone.sh
  docs/      written by ./docs.sh
  .local/    the default install prefix
```

## Clone

```bash
git clone https://github.com/Ghoti-io/suite.git
cd suite
./clone.sh
```

`libraries.txt` is the list: name, remote, branch, dependencies, in build
order. A repository that is already in `../libs` is left alone. `./clone.sh`
says so, and says to run `./pull.sh`, which fetches each one and
fast-forwards `master`.

```bash
./pull.sh
./pull.sh -n
```

## Build and install

A host build needs a compiler, and three of the libraries need more than that.
CTang needs ICU. CJelly needs Vulkan and X11. The tests need GoogleTest.

```bash
sudo apt install build-essential pkgconf libgtest-dev bison flex libicu-dev \
                 libvulkan-dev libx11-dev mesa-vulkan-drivers glslang-tools xxd
```

The default install is the sibling `.local/` directory. It does not need
root, and it does not run `ldconfig`.

```bash
./install.sh
./install.sh BUILD=debug
```

`--global` is `sudo make install` with no `PREFIX`, which is `/usr/local`
and does run `ldconfig`.

```bash
./install.sh --global
```

Uninstall walks the same list from the last library back to the first, and
takes the same flag.

```bash
./install.sh uninstall
./install.sh uninstall --global
```

Each library finds the others through pkg-config. After a `.local` install:

```bash
export PKG_CONFIG_PATH="$PWD/../.local/share/pkgconfig"
```

## Manual

The manual is built in a container, so the documentation toolchain does not
have to be installed on the host. The image has Doxygen, Graphviz, cloc and
Python. The image is rebuilt from the container file on each run; an unchanged
file is cached.

```bash
./docs.sh --container
```

Either Docker or Podman will do. The image is Debian 13, so its Doxygen
matches the page ids in `manual/`. The result is `../docs/html/index.html`.

Line counts, version pins and each library's version are read from the
source. The test totals are filled in once a library has been built; a
library that has not been built shows a dash.

To rebuild the image after the container file changes:

```bash
podman build -t ghoti-io-docs -f Containerfile .
# or: docker build -t ghoti-io-docs -f Containerfile .
```

`./docs.sh` without `--container` runs Doxygen and cloc on the host.

## License

LGPL-3.0-only, the same as the libraries. See `COPYING` and `COPYING.LESSER`.

Patches are not being accepted at this time. See `CONTRIBUTING.md`.
