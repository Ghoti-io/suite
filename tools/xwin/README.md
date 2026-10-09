# Running the Windows branches

`suite/tools/xarch/` answers "what does this code do on a big-endian machine". This
directory answers the same question for Windows, with the same mechanism: a
container holds the cross compiler, the host runs the result. Rootless podman
plus the host's wine, no privileges and no Windows machine.

```bash
./suite/tools/xwin/run-security.sh
```

The toolchain image is `ghoti-cross-mingw64:deb13`, which is suite-wide rather
than per-library; `notes/suite/CONTAINERS.md` section 6 covers the naming and
where it came from. `notes/suite/WINDOWS-TODO.md` is the list of branches that
need this treatment and what a real Windows machine has settled.

## Why this lives in the workspace and not in the library

A library cannot run it. Cross-linking `security` needs **cutil's sources**,
because three cutil symbols are referenced (`gcu_allocator_default`,
`gcu_random_from_engine`, `gcu_random_free`), and pkg-config hands out headers
and a Linux `.so`, not a win64 archive. CONVENTIONS.md's rule that pkg-config
is the only way a library finds another is exactly what stops this from being a
library target: the alternative is a sibling-checkout path inside a published
repository, which is the thing that rule forbids. Two repositories are needed
at once, so it belongs where both are checked out.

A *compile-only* check would need cutil's headers alone and could live in the
library. It would have caught the defect below. Nobody has written it.

## What `run-security.sh` does, and what a green run means

1. Cross-compiles cutil (29 TUs) and security (54 TUs) for win64.
2. Links `probe.exe` from `probe-security.c`, as **archives**, so only the
   cutil members security actually references are pulled in - several of
   cutil's own translation units carry a `main()` or want `-luserenv`.
3. Links `probe_mut.exe`, identical but for one `sed` over `random.c` that
   asks `BCryptGenRandom` for a NULL algorithm handle with no
   `USE_SYSTEM_PREFERRED_RNG` flag, which it must refuse.
4. Runs both under wine. **Exits 0 only when the probe passes and the control
   fails**, because a probe that cannot fail is not evidence.

Security is held to its own warning set and a single warning fails the run.
cutil is not: it is compiled only to be linked against, and security's flags
are not cutil's, so its warnings are printed and not counted.

It proves our code works there - the call, its `NTSTATUS`, the buffer really
being filled, the refusal branches, and `gsec_selftest()` over every
primitive. It proves nothing about a real Windows machine, because under wine
`bcrypt.dll` is wine's own implementation, backed by the host kernel.

## What it found, first run, 2026-09-29

`security` did not compile for Windows at all. `gsec_random_bytes` declared
one `size_t got` at function scope for all three platform arms, and the
`_WIN32` arm has no loop - `BCryptGenRandom` fills in one call - so `got` was
set and never read. That is `-Wunused-but-set-variable`, the library builds
with `-Werror`, and the file was the only one of 54 to warn. The counter now
belongs to the two arms that use it.

The shape is worth remembering because it is the second time in this suite: a
defect that exists **only on the arm no build compiles** (cutil's `mutex.h`
was the first, 2026-09-23). A `_WIN32` branch that no CI touches is not
"probably fine"; it is unproven, and the cheapest thing that could be wrong
with it is that it does not build.

# The runtime stack: `m1-run.sh`

```bash
./suite/tools/xwin/m1-run.sh [scratch-dir]      # default /tmp/xwin-m1; a second run is incremental
```

`run-security.sh` links one library against two of cutil's objects. The runtime
stack is five libraries that depend on each other and on six more
(`runtime-core`, `runtime-heap`, `runtime-jit`, `runtime-debug`, `lang-tang`;
cutil, unicode, chron, regex, text and ctang underneath), and what is wanted is
the libraries' own Makefiles, their own tests and their own gates, run for
win64. So this does not cross-link by hand. It runs each library's real
`make all install test examples` inside `ghoti-cross-mingw64:deb13`, installs
into a prefix in the scratch tree so that the next library finds it through
pkg-config as it would on a Windows machine, and reads the results.

## This imitates MSYS2. It is not Windows.

The Makefiles pick their Windows arm from `uname -s` (`MINGW64_NT*`) and call
`cygpath`. `m1-env.sh`, which `m1-win.sh` sources first, puts a `uname` that
answers `MINGW64_NT-10.0-26100` and an identity `cygpath` ahead of the real ones
in the container, points `cc`, `g++` and `ar` at the mingw compilers, and
arranges for a PE `.exe` that a recipe runs to be run by the host's wine
(binfmt_misc, which needs `--cap-add SYS_ADMIN` and a kernel from 6.7). The host's
python3, bison, flex and m4 are mounted and run in place, since the image has
none. What that gives is the Windows arm of the Makefiles, the mingw-w64 headers
and winpthreads, and wine's implementation of the Windows API. What it does not
give is a Windows C runtime other than the one mingw links, a real console, a
real filesystem (case rules, backslash paths, sharing violations), a real
`HeapAlloc`, MSYS2's `sh` or `patch`, or any behaviour of a real kernel.

## What it runs, in order

1. **Dependencies**: gtest from `/usr/src/googletest`, then cutil, unicode, chron,
   regex, text and ctang (`m1-lib.sh deps`).
2. **For each of the five libraries** (`m1-lib.sh lib <name>`): `make all`,
   `install`, `test` and `examples`, each with `-k` so that one failure does not
   hide the rest; then every test program on its own, so that a crash shows as
   a program and not as a truncated `make test`, with gtest's own counts of
   tests, passes and **skips**; every example and the benchmark smoke, where exit
   status 77 means "skipped"; and a count of compiler warnings. `make test`
   includes the library's gates (`check-gates`, `check-edges`, `check-labels`,
   `check-stamps`, `check-aliasing`, `cli-test`, `fuzz-replay` and so on).
3. **The probe** (`m1-probe.sh`, `probe-runtime.c`): a program linked against the
   *installed* DLLs and import libraries, so that the `dllexport`/`dllimport`
   switching is exercised the way a consumer sees it. It checks the page
   provider (`VirtualAlloc`, `VirtualProtect`, `VirtualFree`, the write-xor-
   execute flip observed with `VirtualQuery`, by running the code, and by a
   write to the read-execute page that must fault), winpthreads (create, join,
   `_Thread_local`, which clock a condition accepts, and a runtime-core request
   port made and posted to), a function compiled by runtime-jit, run and destroyed through the DLLs
   (`grjit_backend_available()` is true; its unwind table is found by
   `RtlLookupFunctionEntry` while the code lives and not after), and
   `grdbg_transport_create_fd` refusing. It is built twice; the second build
   leaves the page read-write, and that one **must fail**.
4. **The controls** (`m1-controls.sh`): for each fix that has no Linux symptom,
   a copy of the built tree with that fix put back, and the suite required to
   notice. The clock fix: `testRequest` and `testProfile` must fail. The stack
   reserve: `tang.exe` and `testCompile.exe` relinked without `--stack` must fail
   on a tree 10,000 deep. Binary standard streams: `tang.exe` without the
   `_setmode` calls must fail the byte checks of `cli-test.sh`. The Windows
   backend of runtime-jit: a scratch copy built with each of three planted
   defects, which the test that *runs* the code must fail on and pass without:
   a callee-saved register used (sentinels in `rbx`, `rsi`, `rdi`, `r12`-`r15`
   change), no outgoing area (a helper's shadow space lands on the caller's
   live slots), and the unwind table never registered (a helper that walks up
   with `RtlVirtualUnwind` finds no frame). Linux catches the same three
   structurally, with runtime-jit's `make check-planted`. The tree under
   test is never edited (a restore after an edit keeps the old mtime, and the
   stale object it leaves behind passes).

## What a green run means

`m1-run: GREEN` is printed, and the exit status is 0, only when, for all five
libraries, every make target returned 0, every test program passed, no example
failed, and the compile produced no warning; when the probe passed and its
control failed; and when every control failed. Skips are reported and are not
failures, but they are counted separately and the SUMMARY line says how many,
because a suite that skips a test has not run it:

| library | what is skipped on Windows, and why |
| --- | --- |
| runtime-jit | six tests, none for want of a backend (Windows x86-64 has one, run under wine): four that read a host `objdump` (POSIX `mkstemp`/`popen`), one that is arm64's instruction-cache count on an arm64 build, and one that needs a target with no unwinder to register with; and `check-planted`, a Linux target (its three Windows defects are run by the controls instead). |
| runtime-debug | the socket-pair and loopback-TCP sessions and the example (the descriptor transport is a documented stub there); the stub's own error return is tested instead. |
| lang-tang | the allocation-failure sweeps (cutil allocates through `HeapAlloc` on Windows, which `--wrap` cannot reach), the ctang child-process driver, `--dap` sessions, the web-server example, `test-oracle` and `check-planted`. |

It proves that our code builds with the full warning set, links, and passes its
own tests against mingw-w64, winpthreads and wine. It does not prove anything
about a Windows machine: wine's `kernel32` and `ntdll` are wine's own, so a
run here answers "does our code work against this API" and not "does the
platform behave as we assumed". `VirtualProtect` returning success under wine
is not evidence about real data-execution prevention; an exception handler
that works under wine has not been shown to work under SEH on Windows. A
`TODO(windows)` marker is discharged by a branch running, and under wine it
has run; the source says "wine", not "Windows", where it says anything.

Last green run, 2026-10-06 (tests ran / passed / skipped; examples ran /
skipped; every library built with no compiler warning). It ran against the
working trees, all clean, at cutil `81ffede`, unicode `d9c40b1`, runtime-core
`40624b8`, runtime-heap `4bcfc9f`, runtime-jit `5dd07f9`, runtime-debug
`bcfb345` and lang-tang `2c2844e`; `m1-run: GREEN`, and every control and the
probe behaved:

| library | tests | examples |
| --- | --- | --- |
| runtime-core | 488 / 488 / 0 | 6 / 0 |
| runtime-heap | 286 / 286 / 0 | 4 / 0 |
| runtime-jit | 185 / 179 / 6 | 4 / 0 |
| runtime-debug | 150 / 148 / 2 | 1 / 1 |
| lang-tang | 555 / 541 / 14 | 10 / 1 |

It was the third attempt. The first two were NOT GREEN, each for one program
that wine itself ended with `wine client error:0: recvmsg: Connection reset by
peer` (the first in lang-tang's `testNative_gate` under `GRHEAP_TORTURE=1`, the
second in runtime-jit's `testUmbrella`); in both the same program passed in the
other attempts and the rest of the run was unchanged, so they are recorded here
as wine-server flakes of this host, not as defects found. They were not
investigated further. The previous green run (2026-10-04, after story 18) was
456, 269, 178, 144 and 547 tests.

The examples column includes the benchmark. `check-symbols` is Linux-only and
is reported skipped by every library's Makefile; it is the one gate this does
not run.

Backend skips are zero for runtime-jit and lang-tang: the JIT tests, the
JIT arm of the frame differential and `jit_hot_loop` run compiled code (the
last shows two functions compiled, four entries and a deopt). Everything above
ran under wine, and a claim about a real Windows machine is not made.

## What it found, first run, 2026-10-04

What follows had no symptom on Linux: winpthreads refuses
`CLOCK_MONOTONIC` for a condition variable and runtime-core read that as out of
memory, so no context could make a request port (and a timed wait there cannot
be monotonic at all); the page test caught a write to read-execute code with
`sigsetjmp`; `check-install.sh` and `check-dynamic-gates.sh` ran the program
they had named and not the `.exe` the compiler had written (all ten checks of
the second failed); runtime-heap's retention test used `open_memstream`;
runtime-jit counted a pass for every test that returned early for want of a
backend, and its tests, examples and benchmark did not build or failed;
runtime-debug's socket tests and example did not compile; lang-tang's
`#define ERROR` collided with `windows.h`, `SIGPIPE` and `alarm` and
`execinfo.h` and `malloc_usable_size` do not exist, `isnan` tripped
`-Wfloat-conversion`, `tmpfile()` returned NULL (it writes to the drive root),
`ls` through `popen` listed no corpus (so a count test failed rather than
passing on nothing), standard output was a text stream (`\r\n`, so every
comparison of a printed line failed), and a tree 10,000 deep ended the process
with status 1 and no message because the default 2 MB stack is not enough for
the recursion the depth budget allows.

## Fibers: `fiber.sh` (cutil, Defiant milestone 1 story 1)

```bash
suite/tools/xwin/fiber.sh [scratch-dir]
```

Cross-builds cutil for win64 in `ghoti-cross-mingw64:deb13` with its own
Makefile, builds gtest from the host's `/usr/src/googletest`, and runs all of
`test-fiber.exe` under wine. It then builds the library twice more, once with
the switch skipping the MXCSR control bits and once skipping the x87 control
word (`GCU_FIBER_PLANT_NO_MXCSR`, `NO_X87CW`), and requires the rounding test to
fail on each. Exit 0 only if the real build passes and both controls fail.

Two things it settled. **`FIBER_FLAG_FLOAT_SWITCH` did not make the rounding
test pass under wine**: with the flag passed, every probe was wrong on every
round, so the Windows arm saves the two registers itself and does not rely on
the flag. And GCC 14 with the MinGW-w64 headers reports `GetCurrentFiber()` as
`-Werror=array-bounds`; the call is wrapped in a pragma. This is wine, not
Windows; `notes/suite/WINDOWS-TODO.md` item 12 says what is and is not known.

## Event loop and sockets: `loop.sh` (cutil, Defiant milestone 1 story 2)

```bash
suite/tools/xwin/loop.sh [scratch-dir]
```

Cross-builds cutil for win64 as `fiber.sh` does and runs all of
`test-socket.exe` and `test-loop.exe` under wine: the Windows arm of
`src/socket.c` and `src/loop.c` (an I/O completion port, `AcceptEx`,
`ConnectEx`, overlapped `WSARecv` and `WSASend`). It then builds the library
three more times, each with one defect compiled in (`GCU_LOOP_PLANT_NO_WAKE`,
`CANCEL_EARLY_RELEASE`, `TIMER_ORDER`), and requires the one test that carries
each to fail by name. Exit 0 only if the real build passes and every control
fails.

What it found: a datagram longer than the buffer is completed twice (an
immediate `WSAEMSGSIZE` *and* a packet), and a cancel of an operation the OS
had already finished succeeded. wine also has no message text for
`WSAEADDRINUSE`. This is wine, not Windows; `notes/suite/WINDOWS-TODO.md` item
14 says what is and is not known.

## HTTP: `http.sh` (http, Defiant milestone 1 story 3)

```bash
suite/tools/xwin/http.sh [scratch-dir]
```

Cross-builds cutil, security and compress (as committed, not as the working
trees stand) and `libs/http` for win64 with their own Makefiles, runs every unit
test under wine (each binary's count must equal the host's; the WebSocket suites
draw masking keys and nonces from `BCryptGenRandom`, so that is also what shows
the Windows entropy arm works under wine), and then runs the oracle's probes
(HTTP/1.1, HTTP/2 and, when `ws_probe.c` and its corpus exist, WebSocket) and a
writer round trip over the corpus `make check-oracle` draws from:
the oracle's probe (every case parsed whole and a byte at a time, ~10,000
lines) and a writer round trip with a 70,001-byte chunked body. Both must answer
**byte for byte as a Linux x86-64 build does**: the reference answers come from
`suite/tools/xarch/http-host.sh`, which it runs first and which checks them against
the Makefile's own probe. It also checks that `parser.o` carries `-export:`
directives and the DLL exports the API, and plants a parser defect, an HTTP/2 defect and a WebSocket defect (an unmasked
client frame accepted) and requires each to be caught. Exit 0 only if every step
held; a missing WebSocket corpus or probe is an error, not a skipped part.

What it found: `getline` is not declared by mingw under `-std=c17`, so the probe
now reads its own lines. This is wine, not Windows.

## Certificate and security: `certificate.sh` (certificate, 2026-10-06)

```bash
suite/tools/xwin/certificate.sh [scratch-dir]          # certificate's 161 tests
WITH_SECURITY=1 suite/tools/xwin/certificate.sh        # and security's 120
```

Cross-builds cutil, `security` and `certificate` for win64 with **their own
Makefiles**, all three as committed (`git archive HEAD`, so another session's
half-edit is not what is tested), and runs the unit tests under wine. It checks
that an object carries `-export:` directives and the DLL exports the API, and
plants a conversion that turns a bad signature into success and requires the
same run to fail. Exit 0 only if every step held.

What it found, first run: `test_selftest.cpp` compared two different enums, which
GCC reports under `-Werror` only when gtest is not a system header; here it is in
the prefix, on Linux it is `/usr/include`. And with `WITH_SECURITY=1`, the first
run of `security`'s own Windows link: `-lbcrypt` sat in `LDFLAGS`, before the
archive that needs it, so no test linked. Both fixed. Not covered: `*_API`'s
dllimport arm (the tests link archives), `make install` into `/mingw64`, real
MSYS2. This is wine, not Windows. Since the same day's additions it also
runs the root-store enumeration (`CertEnumCertificatesInStore`, which links
`-lcrypt32` after the archive): wine's ROOT store held 152 certificates the library
read and 3 it counted as unsupported. The count the script requires is in
`certificate-win.sh` (161: Linux's 162 less the two tests that read a
Unix bundle, plus the Windows one); change it with a test.


## The runtime stack, calls (story 7b of the calls spec, 2026-10-08)

`m1-run.sh` now also holds the runtime stack to what story 7b of the calls spec
asks of Win64:

- **Skips are named and counted.** `m1-lib.sh` lists each skipped test by name
  (gtest's summary lists them) and compares the list with `m1-skips.txt` in both
  directions: a skip that is not in the file fails the run, and so does a listed
  test that ran. Nothing is skipped for want of `fork`: the abort cases of
  `runtime-core` and the forked one of `runtime-jit` run the test binary again
  as a child (`run_in_child` in each `tests/test_helpers.h`).
- **`runtime-heap` is built a second time with `RELOCATE=yes`** in a prefix of
  its own (`/w/prefix-reloc`); every test program runs with `GRHEAP_RELOCATE=1`
  and `check-relocation-present` and `check-relocation-gates` run, and the
  SUMMARY line says how many programs, tests and gate checks.
- **`m1-controls.sh` reads runtime-jit's `tools/win64-plants.txt`** for the
  planted defects of the Windows backend (5 to 7 and 30 to 37): each is built
  into a scratch copy of the cross-built tree, its named test must pass on the
  real executable and fail by an assertion on the planted one. `CONTROLS_ONLY="30
  33"` runs only those (and none of the Windows-fix controls above them).
- `runtime-jit/tools/check-planted-calls.py --target=win64` (run in the
  container, `--prefix=$WPREFIX`) plants edits in the Windows paths of the
  emitter and the unwind table and requires a test to fail on each.

Wine's `ntdll` is wine's own: none of this says what a real Windows kernel's
exception dispatch or guard-page growth does.


## `tls`: `tls.sh` (libs/tls, 2026-10-07)

```bash
suite/tools/xwin/tls.sh [scratch-dir]            # default /tmp/xwin-tls
```

Cross-builds cutil, security, certificate and tls for win64 **with their own
Makefiles** (each taken as committed, `git archive HEAD`) and runs tls's unit
tests under wine through `m1-win.sh`; `tls-win.sh` is the part that runs inside
`ghoti-cross-mingw64:deb13`. It requires the DLL to carry the `dllexport` arm of
`GTLS_API`, every unit test to pass (the count is in `tls-win.sh`, which this run
holds to the host's), and each of a handful of planted defects (the key schedule,
the record nonce, the PSK binder, the early secret's label) to be caught by the
same run. Wine's `bcrypt.dll` and sockets are wine's own: none of it says what a
real Windows machine does.

