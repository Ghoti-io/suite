# Cross-architecture verification container

A throwaway container for answering questions this machine cannot: what the
code does on a **big-endian** target, on a **strict-alignment** target, and in
a **32-bit** build.

It needs no privileges and no binfmt registration. The cross compilers produce
foreign binaries and `qemu-<arch>` runs them explicitly, in userspace, so it
works under rootless podman. `docker` on this host is a podman shim; either
name works.

```bash
podman build -t ghoti-xarch:deb13 -f suite/tools/xarch/Containerfile suite/tools/xarch
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:deb13 bash /work/suite/tools/xarch/smoke.sh
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:deb13 bash /work/suite/tools/xarch/check.sh
```

The tag is written out, and it has to be. `podman build -t ghoti-xarch` is the
easy thing to type and gives you `:latest` silently - which is the one tag the
suite's convention forbids, because a tag that does not say what is inside the
image makes a green run name a set of names rather than a set of bytes. This
image was `:latest` for a day for exactly that reason. `deb13` is the base it
is built from, matching `ghoti-cross-mingw64:deb13`; see
`notes/suite/CONTAINERS.md` section 6.

`smoke.sh` checks the container. `check.sh` is the gate: it exits non-zero if
any target disagrees with the others or with Appleby's published verification
values. `murmur3.sh` is the same run without the verdict, for when you want to
read the numbers. Each takes an optional path to an alternative `string.c`, so
an old revision can be run against the same harness:

```bash
git -C libs/cutil show 6dfce64:src/string.c > /tmp/old.c
podman run --rm -v "$PWD:/work:ro,z" -v /tmp:/proto:ro,z \
  ghoti-xarch:deb13 bash /work/suite/tools/xarch/check.sh /proto/old.c
```

`check.sh` has been verified able to fail, which is the only thing that makes
a green run mean anything: against the byte-order swap removed it reports 8
failing checks, against the pointer casts of `6dfce64` 8, and against the
wrong fmix32 of `1eb60f2` 11.

## The matrix

| target | word | order | note |
|---|---|---|---|
| `x86_64-linux-gnu` | 64 | little | the host, native, the control |
| `i686-linux-gnu` | 32 | little | 32-bit `size_t` |
| `aarch64-linux-gnu` | 64 | little | second little-endian opinion |
| `s390x-linux-gnu` | 64 | **big** | |
| `powerpc64-linux-gnu` | 64 | **big** | |
| `powerpc-linux-gnu` | 32 | **big** | 32-bit *and* big-endian at once |
| `sparc64-linux-gnu` | 64 | **big** | **traps on unaligned access** |

`gcc-multilib` is deliberately absent — it conflicts with the powerpc64 cross,
and `gcc-i686-linux-gnu` covers 32-bit little-endian better. The
`libc6-dev-*-cross` packages are **not** pulled in by the compilers under
`--no-install-recommends`, and without them every cross build dies on
`<stdio.h>`, which reads as a broken toolchain rather than a missing header.

## Run `smoke.sh` first, every time

`smoke.sh` proves each toolchain compiles, each qemu runs the result, and each
target really is the word size and byte order it claims — checked twice, once
by a runtime union probe and once against the ELF header via `file`. A
cross-architecture result is worthless if the "foreign" binary was quietly the
host's, and that failure is silent. It is the control for everything else here.

## What it found the first time it was run

| | |
|---|---|
| `gcu_string_hash_64()`'s 32-bit arm returned `size_t` where `string.h` promises `uint64_t` | cutil **did not compile at all** on any 32-bit target |
| the pre-`6fd9b46` pointer casts in murmur3 | **SIGBUS on sparc64**, dying between the aligned and misaligned call |
| murmur3 output on the four big-endian targets | differed from the three little-endian ones on 35 of 38 measurements; `string.h` documented this and nobody had measured it. Fixed in cutil's `5c5ef85`, and all seven now agree |

The first was invisible because no 32-bit build had ever been attempted; see
`[[unbuilt-preprocessor-branch]]`. The second had been reported by UBSan as
undefined behaviour, but "undefined" and "crashes on a shipping architecture"
land differently, and only this container can show the second.

## Caveat carried by `murmur3.sh`

It compiles against the **host's** generated `float.h`, from
`libs/cutil/build/linux/release/include`. Every target in the matrix is
IEEE-754 with 32-bit `float` and 64-bit `double`, and `string.c` touches
neither, so it is inert here — but it is not a cross-generated header, and
anything that reads those types needs one that is.

## `bench-hash64-split.c`

Answers whether `gcu_string_hash_64()`'s `#if SIZE_MAX` split earns itself, by
calling both 128-bit variants directly at both word sizes. Built `-static` for
i686, which runs natively on an x86-64 host, so this is real hardware rather
than emulation:

```bash
podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xbench:/out:z ghoti-xarch:deb13 bash -c '
  INC="-I /work/libs/cutil/include -I /work/libs/cutil/build/linux/release/include"
  for v in 1 2; do for m in 1 2; do
    i686-linux-gnu-gcc -std=c17 -O2 -static -DVARIANT=$v -DMODE=$m -DSCALE=60 $INC \
      /work/suite/tools/xarch/bench-hash64-split.c /work/libs/cutil/src/string.c -o /out/s32-v$v-m$m
  done; done'
```

`VARIANT` 1 is x86_128 and 2 is x64_128; `MODE` 1 is short keys and 2 is bulk;
`SCALE` multiplies the workload. Check `objdump -d | awk '/<main>:/,/^$/'` names
the variant you meant — static linking puts *both* in the binary, so grepping
the whole file finds the one you did not call.

Result, wall clock, five interleaved paired trials pinned to one core:

| | x86_128 | x64_128 | |
|---|---|---|---|
| 32-bit, short keys | 581.8 ms | 724.1 ms | x86_128 by 24% |
| 32-bit, bulk 64 KB | 93.5 ms | 235.3 ms | **x86_128 by 152%** |
| 64-bit, short keys | 395.1 ms | 336.6 ms | x64_128 by 15% |
| 64-bit, bulk 64 KB | 87.9 ms | 77.5 ms | x64_128 by 12% |

At the original workload the runs were 4-17 ms with 30-100% spread and the
64-bit bulk row came out with the *opposite* sign. `SCALE=60` brings the spread
to 1-2%. Instruction counts and the clock agree on direction everywhere but
not on magnitude for bulk (Ir says x64_128 is 45% cheaper on 64-bit bulk, the
clock says 12%), which is what a bandwidth-bound loop looks like.

## The whole suite: `jit-arm64.sh` (story 17, CAP-14)

The sections above run compiled *objects*. This one builds the **runtime stack
for AArch64 and runs the baseline JIT's suites under `qemu-aarch64`**, because
the question it answers is "does the arm64 backend work", which needs the tests,
not a program.

```bash
podman build -t ghoti-xarch-arm64-jit:deb13-gxx14-gtest \
  -f suite/tools/xarch/Containerfile.arm64-jit suite/tools/xarch
mkdir -p /tmp/xarch-jit
podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xarch-jit:/scratch:z \
  ghoti-xarch-arm64-jit:deb13-gxx14-gtest bash /work/suite/tools/xarch/jit-arm64.sh
```

`Containerfile.arm64-jit` builds on `ghoti-xarch:deb13` and adds a C++ cross
compiler and its libstdc++ (`g++-aarch64-linux-gnu`), `pkg-config`, `bison` and
`flex` (lang-tang generates its parser on the build host), the distribution's
googletest source built into an aarch64 archive with a `gtest.pc` beside it, and
the host's `libgtest-dev` for the x86-64 control. The tag names what is inside
(the parent, the compiler, that gtest is in it) and is never `:latest`; it does
not retag `deb13` over different contents (`notes/suite/CONTAINERS.md` section 6).

What the script does:

1. **Checks the machinery.** An AArch64 binary is AArch64 by its ELF header
   (`readelf` and `file` must agree), does not execute on this host without qemu
   (if it does, "foreign" is a lie and the script stops), and runs under it.
2. **Builds the stack twice** into a scratch directory outside every repository
   and outside `.local/` (`/scratch`, a prefix of its own per build): `cutil`,
   `unicode`, `runtime-core`, `runtime-heap`, `runtime-jit` and `lang-tang`
   (`WITH_DEBUG=no`: the debugger, `text`, `regex`, `chron` and `ctang` are
   needed by no test of the suites below, so they are not built). Once for
   AArch64 (cross compiled; the cross `ar`, `nm` and `objdump` are put first in
   `PATH` because the Makefiles call them by their plain names), once for x86-64
   natively, which is the control and must pass too. cutil makes `float.h` by
   running a program it has just built, which an AArch64 build cannot do on this
   host, so the script runs that program under qemu and writes the header the way
   the Makefile does. Nothing is built in a library checkout (the source is
   copied in).
3. **Runs the suites.** Every test binary of `runtime-jit`, and `lang-tang`'s JIT
   arm: `testJit`, and the Makefile's bounded suites (which include the execution
   corpus, the interpreter-against-JIT frame differential) with
   `GLTANG_TEST_JIT_THRESHOLD=1`, every function tiering up at its first poll.
   Before each binary runs its ELF machine is read again. A non-zero exit stops
   the script.
4. **Plants the arm64 defect** (`SHR` and `SAR` swapped in the arm64 emitter) in
   a tree of its own, and requires the native differential, run under qemu, to
   fail on it while the same test passes without it. A build failure or a crash
   is not a catch.

**Skips are listed by name, with the reason, and counted.** There are four on the
AArch64 build: `Asm.TheSystemDisassemblerReadsTheSameInstructions` (it runs the
host's x86-64 `objdump` through `popen`, which qemu-user cannot exec from an
AArch64 guest; excluded by the script with a filter that names exactly it);
`Win64Shape.TheProbeLoopDisassemblesAsTheSequenceItIsMeantToBe` and
`Win64Registers.TheDisassemblerAgreesThatNoCalleeSavedRegisterAppears` (the same
x86-64 `objdump`, over Win64 prologues and generated functions; the tests skip
themselves); and `AsmArm64.TheRecordedDisassemblyAgreesWithObjdumpWhenTheToolIsPresent`
(it needs the cross `objdump` the same way; the test skips itself). The x86-64
control runs the last of those with `GRJIT_AARCH64_OBJDUMP` set, so every
recorded encoding is re-checked against the real disassembler, and skips one test
that is about the arm64 native backend. The script asserts the number and the
names of the skips on each build; a skip that appears or vanishes fails it.

First run (2026-10-04, a 12-thread Intel Core 7 150U, qemu 10.0): **193 s** with
a warm `/scratch`, 29 test binaries and 538 tests passed under qemu-aarch64
(2 skipped), 539 on the x86-64 control (1 skipped); the planted defect failed at
seed 3 of the differential.

Latest run (2026-10-06, the same machine): **224 s** against the working trees
at cutil `81ffede`, unicode `d9c40b1`, runtime-core `40624b8`, runtime-heap
`4bcfc9f`, runtime-jit `5dd07f9` and lang-tang `2c2844e` (all clean, so these
are the commits): 31 test binaries, 573 tests passed under qemu-aarch64
(4 skipped), 578 on the x86-64 control (1 skipped); the planted defect failed at
seed 3 of the differential. `jit-arm64: OK`. This replaces the first run
(2026-10-04: 193 s, 29 binaries, 538 passed with 2 skipped, 539 on the control),
which predated the runtime-jit and lang-tang commits of 2026-10-04 and 2026-10-05
(the audit that asked for this re-run is `planning/specs/spec-runtime-stack-m1/audit-2026-10-05.md`, item 8).

**What it does not show**, and the script says so when it ends: the instruction
cache (`qemu-user` translates lazily and does not model it, so the library's cache
call is covered by a unit test that checks it is reached, not by this run) and
anything about real arm64 hardware (memory ordering, the real cache, a real
kernel's W^X, unaligned-access cost).

## Scope

`smoke.sh`, `check.sh` and the others above run *compiled objects*, not `make
test`: gtest is not cross-built in the base image. They answer "what does this
code do on that machine". `jit-arm64.sh` and its image cross-build gtest and
answer "does the suite pass there" for AArch64, for the libraries it names.

## Fibers: `fiber.sh` (cutil, Defiant milestone 1 story 1)

Builds cutil's `fiber.c` for aarch64 with the library's own flags and `-Werror`,
builds the plain-C harness `fiber-check.c` against it, and runs the harness
under `qemu-aarch64`: the switch, the callee-saved registers, FPCR isolation,
the guard page and the thread pin. The same harness is built for the host as
the control. Then it plants each defect (`GCU_FIBER_PLANT_NO_FPCR` on aarch64,
`NO_MXCSR` and `NO_X87CW` on the host) and requires the harness to fail, and
finally builds `fiber.c` for every target in the matrix that has no switch
routine and runs `fiber-unsupported.c` to see that it links and refuses.

```bash
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:deb13 bash /work/suite/tools/xarch/fiber.sh
```

It reads the ELF header of the aarch64 binary and requires that it does not
run natively, as `jit-arm64.sh` does.

What it cannot show: the sanitizers do not run under qemu-user, so arm64 has no
sanitizer gate (the x86-64 gates in cutil's `make check-fiber-defects` carry
that); qemu-user is not arm64 hardware, so FPCR is qemu's; and this image has
no C++ cross compiler, so the harness is C and not `test-fiber.cpp`.

## Event loop and sockets: `loop.sh` (cutil, Defiant milestone 1 story 2)

Builds cutil's `loop.c`, `socket.c` and `fiber.c` for aarch64 with the
library's own flags and `-Werror`, builds the plain-C harness `loop-check.c`
against them, and runs it under `qemu-aarch64`: the epoll arm's system calls,
timer order and "not before their time", a write larger than the socket
buffer, the cancel rule, a post from another thread, a fiber waiting on a read,
a datagram and a refused connect. The same harness is built for the host as the
control. Then it plants each defect (`GCU_LOOP_PLANT_NO_WAKE`,
`CANCEL_EARLY_RELEASE`, `TIMER_ORDER`) on both and requires the named check to
fail while the unrelated ones pass, builds the arm kqueue targets get
(`-DGCU_LOOP_FORCE_UNSUPPORTED`) and runs `loop-unsupported.c` to see that it
links and refuses, and builds and runs the harness for the other targets in the
matrix, which are Linux and so have epoll.

```bash
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:deb13 bash /work/suite/tools/xarch/loop.sh
```

Nothing in the harness waits without a bound, so a defect that loses a wake-up
is a failed check and not a hung run. What it cannot show is the same as for
fibers: no sanitizer gate under qemu-user, qemu's epoll is a translation of the
host's, and the harness is C and not `test-loop.cpp`.

## HTTP: `http.sh` and `http-arm64.sh` (http, Defiant milestone 1 story 3, 3c)

```bash
suite/tools/xarch/http-host.sh [scratch-dir]          # all seven targets, the probes
suite/tools/xarch/http-arm64-host.sh [scratch-dir]    # aarch64, the real gtest suites
```

`http-host.sh` writes the differential's corpora (`libs/http/tools/oracle/corpus.py
--dump`, `diff_h2.py --dump`, and `diff_ws.py --dump` once that exists), stages
`git archive HEAD` of cutil, security and compress (so that another session's
half-edited working tree is not what is tested), and runs `http.sh` in this
image. libghttp links all three, so `http.sh` compiles, for every target in the
matrix, the library and ALL the sources of cutil, security and compress, each
with its own library's flags and `-Werror` (cutil's float.h is generated by
running the target's `float_identifier` under qemu). The objects are linked
directly, not through an archive, because compress registers its methods from
constructors. It then runs the HTTP/1.1, HTTP/2 and WebSocket probes (about
10,000 + 7,000 + N lines: every case whole and a byte at a time) and a writer
round trip under qemu-user, and requires each target's output to equal the
host's byte for byte. Planted defects on aarch64 (a parser bare LF, HTTP/2 DATA on
an idle stream, and a WebSocket server accepting an unmasked client frame) must
each make the comparison fail. The WebSocket probe and its corpus (`diff_ws.py --dump`) are required: a
missing one is an error.

`http-arm64-host.sh` runs `http-arm64.sh` in the image with a C++ cross toolchain
and a gtest for the target: it builds cutil, security and compress WHOLE with
their own Makefiles (cutil's `float_identifier` through a qemu wrapper), then
libghttp and every one of its test binaries with its own Makefile, runs them
all under qemu-aarch64, and requires each binary's passed count to equal the
count the HOST's binary of the same name lists (`http-host-counts.sh`). It plants
Content-Length with Transfer-Encoding and an unmasked-client-frame WebSocket
server to see the matching suites go red under qemu. The plain `ghoti-xarch`
image has no C++ cross compiler, which is why the seven-target run is a probe
and not `make test`.
