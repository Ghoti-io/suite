# Cross-architecture verification container

A throwaway container for answering questions this machine cannot: what the
code does on a **big-endian** target, on a **strict-alignment** target, and in
a **32-bit** build.

It needs no privileges and no binfmt registration. The cross compilers produce
foreign binaries and `qemu-<arch>` runs them explicitly, in userspace, so it
works under rootless podman. `docker` on this host is a podman shim; either
name works.

## Which image: `sid-gcc16`, and `deb13` for what has not moved

The suite's compiler is GCC 16.2.0 (`ghoti-build:gcc16`, see CLAUDE.md), and a
32-bit or big-endian result from a different compiler under the same `-Werror`
answers a different question. Debian 13's cross compilers are GCC 14.
**`ghoti-xarch:sid-gcc16`** (`Containerfile.gcc16`) has GCC 16.2.0 for all six
cross targets, from a digest-pinned Debian sid base and a dated
`snapshot.debian.org` archive, so a rebuild gets the same packages. The
compilers are installed as `gcc-16-<triple>` and linked to the `<triple>-gcc`
names every script here calls; `smoke.sh` prints each version.

```bash
podman build -t ghoti-xarch:sid-gcc16 -f suite/tools/xarch/Containerfile.gcc16 suite/tools/xarch
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 bash /work/suite/tools/xarch/smoke.sh
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 bash /work/suite/tools/xarch/check.sh
```

Run on `sid-gcc16` on 2026-10-14 and passing: `smoke.sh`, `check.sh`,
`fiber.sh`, `loop.sh`, `lfs.sh`. The runtime/JIT gates now use
`Containerfile.arm64-jit-gcc16`, a child of that pinned image. The older
`Containerfile.arm64-jit` and `Containerfile.text` remain GCC 14; `http.sh`,
`tls.sh` and `text.sh` have not moved. Their earlier results are GCC 14
results, and a GCC 16 `-Werror` finding while moving them is a real finding.

The original image, still used by the scripts above that have not moved:

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

## Floating point: `xarch_fp_cflags` and `fp.sh`

The i686 target defaults to x87, whose 80-bit registers round a `double` sum
twice and so can give different bits from every other target. A script that
builds code whose `double` results are compared across targets adds
`$(xarch_fp_cflags "$triple")` (in `targets.sh`) to its compile line: it is
`-msse2 -mfpmath=sse` for i686 and empty for the rest (AD-17 and AD-26 of the
shape-family architecture).

`fp.sh` is the gate for that, and `check.sh` runs it. `fp-probe.c` computes a
sum that comes out as `3ff0000000000001` under SSE and `3ff0000000000000`
under x87 (`FLT_EVAL_METHOD` 2). The gate requires every target built with
`xarch_fp_cflags` to print the x86-64 bits; requires the unflagged i686 build to
print different bits and report excess precision (so the probe is known to see
what it exists to see); and runs the gate a second time with the flags
withheld, which must fail. It checks the arithmetic, not the instructions: a
gate that finds no x87 instruction in a build is separate work.

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
podman build -t ghoti-xarch-arm64-jit:sid-gxx16-gtest-wast \
  -f suite/tools/xarch/Containerfile.arm64-jit-gcc16 suite/tools/xarch
mkdir -p /tmp/xarch-jit
podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xarch-jit:/scratch:z \
  ghoti-xarch-arm64-jit:sid-gxx16-gtest-wast bash /work/suite/tools/xarch/jit-arm64.sh
```

`Containerfile.arm64-jit-gcc16` builds on `ghoti-xarch:sid-gcc16` and adds
the GCC 16 host compilers, `pkg-config`, `bison`, `curl` and
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
   and outside `.local/` (`/scratch`, a prefix of its own per build), in order:
   `cutil`, `unicode`, `chron`, `regex`, `text`, `runtime-core`, `runtime-heap`,
   `runtime-debug`, `runtime-jit`, `lang-wasm` and `lang-tang`. `WITH_DEBUG=no`
   applies only to lang-tang; its parser is generated by host `bison` and
   `flex`. Once for
   AArch64 (cross compiled; the cross `ar`, `nm` and `objdump` are put first in
   `PATH` because the Makefiles call them by their plain names), once for x86-64
   natively, which is the control and must pass too. cutil makes `float.h` by
   running a program it has just built, which an AArch64 build cannot do on this
   host, so the script runs that program under qemu and writes the header the way
   the Makefile does. Nothing is built in a library checkout (the source is
   copied in).
3. **Runs the suites.** Every test binary of `runtime-core`, `runtime-heap`,
   `runtime-debug`, `runtime-jit` and `lang-wasm`, and `lang-tang`'s JIT
   arm: `testJit`, and the Makefile's bounded suites (which include the execution
   corpus, the interpreter-against-JIT frame differential) with
   `GLTANG_TEST_JIT_THRESHOLD=1`, every function tiering up at its first poll.
   Before the Lang-Wasm WAST binary runs, the script fetches and verifies every
   file of runtime-jit's pinned WebAssembly spec corpus and supplies the same
   corpus and classification inputs exported by Lang-Wasm's Makefile.
   Before each binary runs its ELF machine is read again. A non-zero exit stops
   the script. The runner clears inherited Google Test filters, sharding and
   repeat settings and selects the full suite, apart from the exact named
   AArch64 exclusions recorded below; a caller's environment cannot silently
   turn this gate into a partial test run.
4. **Builds the relocating heap** into a separate prefix and runs its
   relocation suite, then builds Lang-Wasm's `testDebug_relocate` against that
   prefix. The nested debugger-frame differential compares every instruction
   poll under GC torture, always-move stack mode and live-object relocation.
5. **Plants the arm64 defect** (`SHR` and `SAR` swapped in the arm64 emitter) in
   a tree of its own, and requires the native differential, run under qemu, to
   fail on it while the same test passes without it. A build failure or a crash
   is not a catch.

**Skips are listed by name, with the reason, and counted.** Lang-wasm has no
expected platform-specific skips; any skip from one of its test binaries fails
the asserted count. The AArch64 build has one that the script
excludes with a filter that names exactly it, `Asm.TheSystemDisassemblerReadsTheSameInstructions` (it runs the
host's x86-64 `objdump` through `popen`, which qemu-user cannot exec from an AArch64 guest), and 26 that the tests
skip themselves, each with its reason in `SKIPPED_BY_TEST_AARCH64`: six that need an `objdump` through `popen`
(the Win64 prologue and register reads, the cross `objdump` over the recorded encodings and over the bytes of every
float function); the three probes in x86-64 SysV assembly (`FloatHelpers.*`); the four that run Microsoft x64 code
under wine (`Win64Helpers.*`, `Win64Sentinels.*`); and the 13 `testCpu_features` tests that need a feature arm, which
arm64 does not have (it needs none). The x86-64 control build skips 10: `Arm64Memory...`, the three AAPCS64 probes,
the `fpcr` read and the `d8`-`d15` sentinel caller (arm64 Linux only), and the four Microsoft x64 ones. It runs with
`GRJIT_AARCH64_OBJDUMP` set, so every recorded encoding is re-checked against the real disassembler and the bytes of
every float instruction are read for a callee-saved vector register. The script asserts the number and the names of
the skips on each build; a skip that appears or vanishes fails it. (Story 5 of the float spec removed 36 entries that
said the integer and float code was x86-64 only: `testInt_ir`, `testFloat_ir`, `testDifferential` and `testWast` now
run on arm64.)

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
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 bash /work/suite/tools/xarch/fiber.sh
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
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 bash /work/suite/tools/xarch/loop.sh
```

Nothing in the harness waits without a bound, so a defect that loses a wake-up
is a failed check and not a hung run. What it cannot show is the same as for
fibers: no sanitizer gate under qemu-user, qemu's epoll is a translation of the
host's, and the harness is C and not `test-loop.cpp`.

## Large files: `lfs.sh` (cutil, 32-bit targets)

Builds cutil's `file.c`, `dir.c`, `mmap.c` and their two dependencies for i686
and 32-bit PowerPC with the library's own flags and `-Werror`, and runs the
plain-C harness `lfs-check.c` under qemu-user: list a directory, seek, write
and tell at 3 GiB, `stat` the size past 2 GiB, read back from the end, and map
a 5 GiB sparse file, which a 32-bit build must refuse. The x86-64 host is the
control. Two plants follow: the library built without
`-D_FILE_OFFSET_BITS=64` must fail to compile, naming the flag, and the same
build with that refusal switched off must fail the harness. The directory
listing is held to the plant only if a probe built without the flag fails with
`EOVERFLOW` on the workspace mount: only a directory whose entries carry 64-bit
cookies (ext4) shows it, and tmpfs does not.

```bash
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 bash /work/suite/tools/xarch/lfs.sh
```

qemu-user is not real 32-bit hardware, and the sanitizers do not run under it.

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

## `tls`: `tls-host.sh` and `tls.sh` (libs/tls, 2026-10-07)

```bash
suite/tools/xarch/tls-host.sh [scratch-dir]      # default /tmp/tls-xarch
```

`tls-host.sh` stages cutil, security, certificate and tls **as committed** (`git
archive HEAD`, because another session may be mid-edit in one of them) and runs
`tls.sh` in this image. `tls.sh` compiles all the sources of the four libraries
for each of the seven targets, with each library's own flags and `-Werror`, and
links `tls-probe.c`, which drives RFC 8448's traces (the client's handshake,
HelloRetryRequest, the record layer, the server's flight, the resumed handshake
and the 0-RTT one), a real client against a real server, tickets, resumption and
early data, then requires **every target's output to equal the host's byte for
byte** and every check on it to hold; planted defects on aarch64 must each make
the comparison fail. It is not `make test`: the probe is a program, not the unit
tests, because the plain image has no C++ cross compiler.


## CPU features under qemu CPU models: `jit-x86-cpu.sh` (runtime-jit, 2026-10-10)

`libs/runtime-jit` uses SSE4.1 (`roundsd`), POPCNT, LZCNT and BMI1 (`tzcnt`) where it detects them. On a CPU
without LZCNT or BMI1 the instructions do not fault: they run as `bsr` and `bsf`, with other answers, so a wrong
detection gives wrong results and only a CPU that lacks the feature can show it. This runs the library's
`testCpu_features` under `qemu-x86_64 -cpu <model>` in the same image as `jit-arm64.sh`, which already has
`qemu-x86_64`, GCC 16.2.0, `pkg-config` and googletest for x86-64. The gate
also builds Lang-Wasm's pinned `JitCache.ModulePolicySelectsBaselineOrHostSupportedCpuFeatures`
fixture and runs both module policy masks under every model. It asserts one
executed test, zero skips, the model's detected/effective bits, and the
features recorded in the compiled Wasm code.

```bash
mkdir -p /tmp/xarch-x86-cpu
podman run --rm -v "$PWD:/work:ro,z" -v /tmp/xarch-x86-cpu:/scratch:z \
  ghoti-xarch-arm64-jit:sid-gxx16-gtest-wast bash /work/suite/tools/xarch/jit-x86-cpu.sh
```

Seven models: `qemu64` (none of the four), `Nehalem` (SSE4.1 and POPCNT), `Haswell,-sse4.1`, `Haswell,-popcnt`,
`Haswell,-bmi1,-bmi2` (not plain `Haswell,-bmi1` or `Haswell,-abm`: under those the image's glibc, whose AVX2 string functions use BMI1 instructions, dies at startup or silently misbehaves, and gtest registers no tests), `Haswell,-abm,-bmi2` and `max` (the control, all four). In order it: (1) builds `x86-cpu-probe.c`, which reads CPUID
and runs the four instructions by hand and shares nothing with the library, and requires what it reads under each model
to equal the `MODELS` table in the script (**data about qemu, not about us**: when qemu changes, this stage fails and says which
cell moved, before any claim about the library); (2) builds cutil, runtime-core and runtime-jit natively and runs
`testCpu_features` once (the control); (3) runs it under each model with the expected set in
`GRJIT_TEST_EXPECT_FEATURES`, requiring the detected set to equal it, code compiled for it to be right, and a forced `lzcnt` arm to
give `clz(0x10)` of 4 (the CPU ran it as `bsr`) where the model lacks LZCNT; (4) plants, one tree each, the defects in
`libs/runtime-jit/tools/x86-cpu-plants.txt` and requires the model test to fail by an assertion with its control passing;
(5) asserts, per model, which tests were skipped by name and how many passed.

`Haswell,-bmi1,-bmi2` and `Haswell,-abm,-bmi2` are **crossed** in qemu 10.0.13: removing BMI1 makes `lzcnt` run as `bsr`
while its CPUID bit stays set, and removing ABM makes `tzcnt` run as `bsf` while the BMI1 bit stays set. Detection is
asserted against the bits there, and the forced-arm control against the measured behaviour, but the compiled-code assertions
are skipped by name (`GRJIT_TEST_CPU_CROSSED=1`) and counted. The script derives which models are crossed from the probe and requires
the derivation to equal its table. `JIT_X86_CPU_MODELS="qemu64 Haswell,-bmi1,-bmi2"` runs a subset as a trial of the pieces; the
result then says `TRIAL` and never prints `jit-x86-cpu: OK`. It does not show a real CPU of any model (qemu's models are its
own idea of them) or an operating system's gating of features.

## Running the cross-architecture gates on the EVO-X2 (2026-10-10)

`jit-arm64.sh`, `jit-x86-cpu.sh` and `../xwin/m1-run.sh` do not need this machine. Each can run as a job of its
own on the EVO-X2 through `tools/evo`, in parallel, instead of one after another on the laptop. The workspace the
gates read is shipped as `ws/libs/<lib>` (the thirteen libraries of the runtime stack, including security and lang-wasm) and `ws/suite`, each the working
tree minus `build/`, `docs/`, `.git` and `__pycache__` (about 0.9 GB; ignored third-party and tzdata files go along,
as they do in the local copy). The box has docker and no qemu, wine, podman or bison; everything the gates need is in
three images, built on the box from Containerfiles in this repository:

| image (on the box) | Containerfile | used by |
| --- | --- | --- |
| `localhost/ghoti-xarch:sid-gcc16` | `xarch/Containerfile.gcc16` | pinned GCC 16 parent of the next |
| `localhost/ghoti-xarch-arm64-jit:sid-gxx16-gtest-wast` | `xarch/Containerfile.arm64-jit-gcc16` | `jit-arm64.sh`, `jit-x86-cpu.sh` |
| `localhost/ghoti-xwin-wine:deb13-mingw-win32-wine10-wast` | `xwin/Containerfile.wine` | `m1-run.sh` |

Build the GCC 16 parent and its child on EVO-X2 from their pinned
Containerfiles before the cross gates. The Wine image is rebuilt separately;
its mingw-w64 compiler is a different toolchain family.
Both cross images include `curl` so the gate can fill a missing WebAssembly
spec corpus. Each gate verifies the pinned SHA-256 manifest before running
Lang-Wasm's WAST suite. The Wine gate passes `Z:\w\...` paths to the Windows
test executable through Wine's root-drive mapping, both from `make test` and
from its separate direct-binary run.
For a targeted AArch64 rerun after a WAST/JIT change, set
`JIT_ARM64_FOCUS_WAST=1` on `jit-arm64.sh`; it builds the same target stack and
runs only Lang-Wasm's JIT cache and pinned WAST binaries. It reports a focused
result and leaves the full cross gate to a separate run. To isolate Tang JIT
compatibility on AArch64, `JIT_ARM64_FOCUS_TANG=1` builds the stack and runs
only Tang's `testJit`; this also reports a focused result.
To build or remove (`tools/evo image ls` lists them):

```bash
tools/evo image build localhost/ghoti-xarch:sid-gcc16 suite/tools/xarch suite/tools/xarch/Containerfile.gcc16
tools/evo image build localhost/ghoti-xarch-arm64-jit:sid-gxx16-gtest-wast suite/tools/xarch suite/tools/xarch/Containerfile.arm64-jit-gcc16
tools/evo image build localhost/ghoti-xwin-wine:deb13-mingw-win32-wine10-wast suite/tools/xwin suite/tools/xwin/Containerfile.wine
tools/evo image rm localhost/ghoti-xwin-wine:deb13-mingw-win32-wine10-wast     # likewise the other two, the child first
```

One job per gate (the cap is the job's; with 20G each the three fit under the box's 64G total; `--cpus=8` and
`JOBS=8` keep each to a quarter of the threads, and the gates peak at about 6 GB, 1 GB and 8 GB):

```bash
P='; rc=$?; cp -r /w/logs $RESULTS/ 2>/dev/null; cat /sys/fs/cgroup/memory.peak > $RESULTS/mem-peak.txt; date +%s > $RESULTS/ended; exit $rc'
A=localhost/ghoti-xarch-arm64-jit:sid-gxx16-gtest-wast
J=$(tools/evo new --xgates arm64);  tools/evo run $J -m 20G -t 4h --image $A --mount ws:/work:ro --mount scratch:/scratch --docker-arg --cpus=8 -- "JOBS=8 bash /work/suite/tools/xarch/jit-arm64.sh$P"
J=$(tools/evo new --xgates x86cpu); tools/evo run $J -m 20G -t 2h --image $A --mount ws:/work:ro --mount scratch:/scratch --docker-arg --cpus=8 -- "JOBS=8 bash /work/suite/tools/xarch/jit-x86-cpu.sh$P"
J=$(tools/evo new --xgates win);    tools/evo run $J -m 20G -t 4h --image localhost/ghoti-xwin-wine:deb13-mingw-win32-wine10-wast \
    --mount ws:/work:ro --mount scratch:/w --mount ws/suite/tools:/tools:ro --docker-arg --security-opt=seccomp=unconfined --docker-arg --cpus=8 \
    -- "M1_IN_IMAGE=1 bash /work/suite/tools/xwin/m1-run.sh /w$P"
tools/evo status; tools/evo logs JOB; tools/evo fetch JOB     # the gate's output is the log; results/ has mem-peak.txt and (win) logs/
tools/evo clean JOB
```

`$J` is the job to name in `logs`, `fetch` and `clean`. The first two exit with the gate's status; the log is the same
text the local run prints. The win gate runs `m1-run.sh` inside the image (`M1_IN_IMAGE=1`): `m1-win.sh` then starts no
container, and uses `unshare --user --map-root-user` so that the binfmt_misc entry for wine is private to the gate (it
needs `seccomp=unconfined` for `unshare`, and no capability, and it changes nothing on the box). It unsets `TMPDIR`,
because wine 10.0 frees what `getenv("TMPDIR")` returns and aborts with `free(): invalid pointer` when it makes a prefix.

When `jit-arm64.sh` sees a failing test binary, it also retains that binary's
complete output under `$RESULTS/failures/<architecture>/<library>-<binary>.log`
before printing the concise tail to the job log. This keeps the failing
assertions available in the fetched result bundle for triage.

## text: the XML, HTML, Encoding and markup tests for AArch64 and 32-bit x86 (`text.sh`, 2026-10-14)

`text.sh aarch64|i686` cross-builds cutil, unicode, chron, regex, security and text for the target with their own Makefiles, then
builds and runs, under `qemu-aarch64` or `qemu-i386`, every test program of text whose name starts with `Html`, `Xml`,
`Xhtml`, `Encoding`, `Markup` or `Quadratic` (43 programs; `TEXT_XARCH_TESTS` changes the pattern). It is the same kind of
script as `jit-arm64.sh` and shares its rules: every binary's ELF header is read before it runs, a program that passes no
tests fails, a failure does not stop the others (the list of failures is the last thing printed), and skips are listed by name.
On i686 it also requires that `HtmlSourceMapLimit` ran and did not skip (the 32-bit limit of the offset map, 128 MiB: 2^27
bytes; the test feeds that many bytes and one more), and on AArch64 that it skipped. Its image is
`Containerfile.text` (the arm64-jit image plus `g++-i686-linux-gnu`, and a googletest with `gtest_main` for both targets):

```bash
tools/evo image build localhost/ghoti-xarch-text:deb13-gxx14-gtest-main suite/tools/xarch suite/tools/xarch/Containerfile.text
P='; rc=$?; cp -r /scratch/*/build-*.log $RESULTS/ 2>/dev/null; date +%s > $RESULTS/ended; exit $rc'
for a in aarch64 i686; do
  J=$(tools/evo new --ws text-x$a cutil security unicode chron regex text)
  tools/evo run $J -m 10G -t 5h --image localhost/ghoti-xarch-text:deb13-gxx14-gtest-main --mount ws:/work:ro --mount scratch:/scratch \
      --docker-arg --cpus=6 -- "JOBS=6 bash /work/suite/tools/xarch/text.sh $a$P"
done
```

Under qemu the build takes about eight minutes and the tests about half an hour (`testEncodingCjk` alone is seven minutes: it
sweeps every byte sequence). `GTEXT_TEST_SLOWDOWN` (40 by default here) multiplies the one wall-clock bound in the programs,
`testEncodingFuzzSeeds`' replay, which qemu-user runs in 74 s against its 60 s; `TEXT_XARCH_FULL=1` also sets `GTEXT_TEST_FULL`.

**What the first i686 run found** (two defects of the tests, one of the library's build): a 32-bit program without
`_FILE_OFFSET_BITS=64` gets `EOVERFLOW` from `readdir()` and `stat()` on a file system with large inode numbers, which
the workspace's has, so the tests that list a corpus directory saw no files; text's Makefile now defines it
(`LFS_FLAGS`); two tests (`(size_t)1 << 40`) did not compile with a 32-bit `size_t` and now use the type's own width.
cutil's file layer has the same exposure and is for cutil's session.

## archive: `archive.sh` and `Containerfile.archive` (archive phase I, 2026-10-20)

`archive.sh i686|s390x|powerpc|sparc64|all` builds cutil, security, compress
and archive for the target with the image's cross GCC 16, runs every archive
test program under qemu-user from the library's directory (so the fixtures are
found), then writes a tar, a tar.xz, a zip and a 7z with the examples and lists
each with the others. A program that passes no tests is a failure, and skips
are listed by name. The image is `localhost/ghoti-xarch-archive:sid-gcc16-gtest`,
`Containerfile.archive` over `sid-gcc16` with googletest built for each target;
build it on the EVO and run it there (`JOBS=6`), not here.

The first run (2026-10-20) found two 32-bit build failures in compress (an
lz4 `-Wtype-limits` comparison and `_mm_cvtsi64_si128` in the PCLMUL CRC-64)
and test objects that found gtest's headers only by luck; then i686 767
tests, s390x, PowerPC and SPARC64 766 each, green. The Windows counterpart is
`suite/tools/xwin/archive-run.sh`, run through `m1-lib.sh` in
`ghoti-xwin-wine:deb13-mingw-win32-wine10`; its non-ASCII names needed the wine
shim's `LANG=C.UTF-8`.

## The shape family's cross-target gates: `shape.sh` (shape-family milestone 1, story 2)

The shape libraries (`math`, `shape-geometry`, `shape-scene`, `shape-cpu`) must
produce the same bits on every host (AD-17). The gates that need no cross
compiler run in each library's own `make test` (`check-fp-contract`,
`check-edges`, `check-libm`, `check-no-x87`, with `check-gates` planting a
violation for each). `shape.sh` holds the three that do:

```bash
podman run --rm -v "$PWD:/work:ro,z" ghoti-xarch:sid-gcc16 bash /work/suite/tools/xarch/shape.sh
```

1. **The four copies of each gate script are the same text.** CONVENTIONS
   reuses one script per library; a copy that has drifted is named. Planted: a
   changed copy.
2. **The cross-target hash.** `shape-hash-probe.c` computes a few dozen doubles
   and prints an FNV-1a 64 hash of their little-endian bytes. Every target in
   `targets.sh` must print x86_64's hash when built with `-ffp-contract=off` and
   `xarch_fp_cflags`. Controls: i686 without the SSE flags (x87) and aarch64
   with contraction on (a fused multiply-add) must print other hashes, which
   is what shows the probe can see what the flags prevent. Planted: one
   differing bit on each target in turn; the gate must fail naming that target
   and no other. Windows is `suite/tools/xwin/shape.sh`.
3. **No x87 in the i686 build.** Each library's sources are built for i686
   with the SSE flags and read by that library's own `tools/check-no-x87.sh`.
   The i386 calling convention returns a `double` in `st(0)`, so a function
   returning one holds an `fld` even when built with SSE; the gate allows
   those loads and stores (and `fldz`, `fld1`, `fstp %st(0)`) and refuses all
   x87 arithmetic, `long double`, integer conversions and the control word. A
   control shows the allowed case is real; the plants are the fixture built with
   `-mfpmath=387` and a hand-written `faddp`.

4. **The math corpus hash** (story 3). `libs/math/tools/corpus-hash.c`, built
   with `libs/math/src/elementary/trig.c`, runs `gmath_sin`, `gmath_cos` and
   `gmath_tan` over `libs/math/tests/corpus` (46,569 inputs) and must print the
   hash committed in `libs/math/tests/corpus/trig.hash` on every target. The
   same two controls (i686 without SSE, aarch64 with contraction on) print other
   hashes, and the same plant (a bit flipped on each target in turn) is caught.
   Section 3 also builds `trig.c` for i686 and reads it for x87.

7. **The shape-cpu coverage hash** (story 9). `libs/shape-cpu/tools/corpus-hash.c`,
   built with every `src/*/*.c` of `shape-cpu`, `shape-geometry` and `math` and the
   cutil allocator stand-in, fills random polygons under both fill rules, random
   curves, rectangles with an open subpath and shapes moved to the corners of the
   26.6 range, hashes each coverage in the library's own spelling and must print
   the hash committed in `libs/shape-cpu/tests/corpus/coverage.hash` on every
   target. The coverage path has no floating point, so the controls of section 4
   (x87, a fused multiply-add) could not differ; the control here is the probe
   built to fold its results in the host's byte order, which prints the committed
   hash on a little-endian target and another on a big-endian one. The plant is
   as in section 4. Windows is
   `suite/tools/xwin/shape.sh`, which runs the same probe under wine.

8. **The shape-scene hash** (story 10). `libs/shape-scene/tools/corpus-hash.c`,
   built with every `src/*/*.c` of `shape-scene`, `shape-geometry` and `math` and
   the cutil allocator stand-in, interns 64 random paths (each twice) and 24
   glyph runs of both sources, records 40 lists of every kind of item (a third
   frozen into pictures that later lists instance) and hashes the resource
   hashes, list hashes, counts, expansions, picture depths and the store's
   counters in `shape-geometry`'s spelling; it must print the hash committed in
   `libs/shape-scene/tests/corpus/scene.hash` on every target. The maps are the
   composition of two general maps with entries over 1000 (sums of two products
   of inexact doubles), so
   the controls of section 4 (i686 without SSE, aarch64 with contraction on)
   and the plant apply. From story 11 the library uses cutil's `GCU_MUTEX_T` and
   `GCU_Atomic_Size`, so this probe, and the section 3 and 5 builds of the scene's
   sources, use cutil's real headers (with its generated `libver_gen.h`) and its
   `allocator.c` and `atomic.c`, never the stand-in (the real headers rename the
   stand-in's names, so the two cannot share a translation unit). Since
   story 11 the probe also builds a retained scene (nodes with keys, hidden
   definitions and instances of them, a merge of sibling edits, a diff) and hashes
   its version hashes, IDs, emitted lists, pictures, every occurrence's instance
   path key and composed map, and the damage rectangles of a diff through a
   general map; the composed maps and the damage are floating point, so the
   controls see them too.

Arm64 is qemu-user here, not hardware; the real Pi is `tools/evo --box pi4`. A
library whose sources include its dependencies' headers needs
`SHAPE_DEP_INCLUDES` set to their `-I` flags; the scaffolds include none.
