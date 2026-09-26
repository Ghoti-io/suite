# Ghoti.io

These instructions are for cloning, compiling, updating, and rendering documentation for the [Ghoti.io](https://github.com/Ghoti-io) family of libraries.

Each library is in its own repository.  This set of instructions, then, is to make it easier to work with all of them.

## Project Structure

When everything is checked out, the project tree looks like this:

```
<parent>/
  suite/     this repository
  libs/      cloned by ./suite/clone.sh
  docs/      written by ./suite/docs.sh
  .local/    the default install prefix
```

By convention, `<parent>` will be named `ghoti.io`.

The parent directory will hold `suite/` (which coordinates all of the libraries), `libs/` (which holds all of the actual libraries), and `docs/` (to hold the generated documentation).  Compiling and installation will go into `.local/` by default, but the libraries can also be installed globally (see below).

## Clone

If building from source (or developing), you need to have a convenient way to get all of the libraries from their respective repos.  I tried to make it easy.

Make a parent directory and clone this repository into it. `ghoti.io` is the name of the parent directory.

```bash
mkdir ghoti.io
cd ghoti.io
git clone https://github.com/Ghoti-io/suite.git
cd suite
./clone.sh
```

Inside the `suite/` directory, `libraries.txt` is the list of libraries: name, remote, branch, and dependencies, in build order.  A repository that is already in `../libs` is left alone.  `./clone.sh` says so, and says to run `./pull.sh`, which fetches each one and fast-forwards `master`.

```bash
./pull.sh
```

If you only want the script to fetch and list the commits that **would be** fast-forwarded (but not actually applied), then run `./pull.sh -n` instead.

## Build And Install

Compiling source is always a headache.  This process is not containerized yet, but it is on my todo list.

When compiling, a host build needs to have a compiler, as well as a couple of dependencies required by some of the libraries.  CJelly needs Vulkan and X11.  The tests need GoogleTest.  Chron's optional format check looks for ICU; none of the libraries link it.

```bash
sudo apt install build-essential pkgconf libgtest-dev bison flex libicu-dev \
                 libvulkan-dev libx11-dev mesa-vulkan-drivers glslang-tools xxd
```

### Local/Development Installation

The default install is the sibling `.local/` directory.  This method does not need root, and it does not run `ldconfig`.

```bash
./install.sh
```

If creating a debug build, then use the appropriate command line option.

```bash
./install.sh BUILD=debug
```

### Global Installation

`--global` is essentially `sudo make install`, installing to `/usr/local` and running `ldconfig` so that the libraries are properly registered.

```bash
./install.sh --global
```

### Uninstall

Uninstall walks the same list from the last library back to the first, and uses the same flag.

```bash
./install.sh uninstall
./install.sh uninstall --global
```

### Referencing The Libraries

If you are trying to compile your own program, you will have to tell the compiler/linker where these libraries are.  We use `pkg-config` for this, and the `.pc` files were installed as part of the install process above.

If you install globally, then the package config files are installed in the proper place for a Debian system.  If you installed locally, you will need to set an environmental variable so that package config knows where to look for your files.  For example, after a `.local` install:

```bash
export PKG_CONFIG_PATH="$PWD/../.local/share/pkgconfig"
cc your_program.c $(pkg-config --cflags --libs ghoti.io-tang-0)
```

Of course, this example assumes that you are in a directory that is a sibling to `.local`.

## Manual

The manual is built in a container, so the documentation toolchain does not have to be installed on the host.  The image has Doxygen, Graphviz, cloc, and Python.  The image is rebuilt from the container file on each run; an unchanged file is cached.

```bash
./docs.sh --container
```

Either Docker or Podman must be installed in order to use this method.

Line counts, version pins, and each library's version are read from the library source.  The test totals are filled in once a library has been built; a library that has not been built shows a dash.

To rebuild the image after the container file changes:

```bash
podman build -t ghoti-io-docs -f Containerfile .
# or: docker build -t ghoti-io-docs -f Containerfile .
```

`./docs.sh` without `--container` runs Doxygen and cloc on the host.

## License

LGPL-3.0-only, the same as the libraries. See `COPYING` and `COPYING.LESSER`.

Patches are not being accepted at this time. See `CONTRIBUTING.md`.
