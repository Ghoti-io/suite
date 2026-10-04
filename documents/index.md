# Ghoti.io (it's pronounced "[Fish](https://en.wikipedia.org/wiki/Ghoti)")

This is a suite of cross-platform C libraries.

There are also puns.  Please forgive me.

Clone the libraries, build them, and build this manual from
[Getting Started](@ref getting_started).

## Libraries

| Library | What it implements |
| --- | --- |
| [Archive](@ref archive) | Tar, read and written: v7, ustar, GNU, and pax, from a file or a pipe, with member names classified rather than rewritten. Zip, read and written, stored and deflate, plus zstd and ZipCrypto on read. |
| [Audio](@ref audio) | WAV, AIFF, and FLAC, read and written, with tags and cover art; MPEG audio and Vorbis, read; Opus identified. |
| [Chron](@ref chron) | Civil time, calendars, instants, durations, time zones, and the text formats for them. |
| [CJelly](@ref cjelly) | A Vulkan GUI: native windows, and a renderer for panels, an image, and a Wavefront model. |
| [Color](@ref color) | A colour engine: a colour described exactly, an ICC profile parsed and written, and samples transformed between two spaces, including CMYK LUT profiles and PQ/HLG. |
| [Compress](@ref compress) | Deflate, zlib, gzip, Brotli, LZ4, LZW, RLE, and zstd. |
| [CTang](@ref ctang) | A template language for a host program, with an x86-64 JIT and a bytecode fallback. |
| [CUtil](@ref cutil) | An allocator, containers, checked arithmetic, threads, and the filesystem. |
| [Font](@ref font) | sfnt outlines (`glyf` and CFF) and Type 1, rasterised to coverage, plus the bitmap formats PCF, BDF, PSF, and Unifont hex. |
| [Image](@ref image) | PNG, including APNG, JPEG, BMP, GIF, ICO, WebP, and TIFF. |
| [Lang-tang](@ref lang-tang) | The Tang engine of the language runtime stack, replacing CTang: the parser and syntax tree ported from CTang, a compiler and interpreter that run a template or a script under fuel, memory and depth budgets, a host API (libraries, an error list, template calls with budget scopes), the divergence ledger and the oracle that compare it with CTang, and a `tang` command that runs what it parses and, with `--dap`, serves a debugger. A web-server example serves templates one context per request and lets a template be stepped over the Debug Adapter Protocol. The library depends on runtime-jit for its baseline JIT, which compiles hot integer and boolean code to machine code and returns to the interpreter's exact frame when a guard fails (`JIT=no` builds the interpreter-only engine, which links nothing of it); a paused or new execution can be frozen into a snapshot and restored into a fresh context on any thread to finish as an uninterrupted run does, so many contexts can start from one warmed-up state; it does not depend on the debugger, and the command and the example do, through runtime-debug. |
| [Model](@ref model) | Wavefront OBJ and MTL, STL in both spellings, and Geomview OFF. |
| [Regex](@ref regex) | Regular expressions. Which of the eighteen named dialects compile is measured with the rest of the figures below. |
| [Runtime-core](@ref runtime-core) | The core of the language runtime stack: the execution context and the frame protocol that engines run on. The version, the result vocabulary, the execution context and the frame protocol, and the gates that keep its layering, and, for the JIT, reference-counted compiled code and a reader and writer of a native frame by its metadata, and, for snapshots, hooks on a key and an immutable, reference-counted snapshot of a paused or idle context that is restored atomically, and a sampling profiler that counts, by source location, where a running guest is at its polls (self and inclusive), driven by a request or an optional timer thread. |
| [Runtime-debug](@ref runtime-debug) | The debugger of the language runtime stack: one debug model of breakpoints, stepping and the state of a stop, attached to a runtime-core context, and a Debug Adapter Protocol adapter over a transport the host binds, so an editor such as VS Code can stop a program at a line and show its stack and variables. |
| [Runtime-heap](@ref runtime-heap) | The collector of the language runtime stack: a precise, non-moving mark and sweep heap that attaches to a runtime-core context, with objects described by type descriptors, roots, handles, pins and an arena mode. It works for a C program with no engine, it can write an image of the objects a context reaches, with no address in it, and instantiate it into a fresh heap, and it can say why an object is still alive: the shortest chain of references from a root. |
| [Runtime-jit](@ref runtime-jit) | The baseline JIT of the language runtime stack: a low-level IR with a builder, a verifier and a printer, an x86-64 backend and an arm64 backend, each with its own assembler, and the stack maps and deoptimization records it emits in runtime-core's format, in pages taken from a context's counting page provider and never writable and executable at once. |
| [Security](@ref security) | SHA-256, SHA-384, SHA-512, SHA-1, and MD5, with HMAC, HKDF, and PBKDF2; AES, ChaCha20-Poly1305, Triple DES, RC2, and RC4; X25519, Ed25519, P-256, P-384, and RSA; Argon2, scrypt, and bcrypt; and DER, PEM, PKCS#8, PKCS#12, X.509, a certificate revocation list, and a basic OCSP response. |
| [Text](@ref text) | JSON, including JSON Schema, and CSV, YAML, TOML, and INI. |
| [Unicode](@ref unicode) | Character properties, the four normalisation forms, segmentation, the bidirectional algorithm, case mapping, and character names. |

The size of each library, the tests its build lists, and the versions its
checks are pinned to are on the [measured](@ref suite_measured) page. They
are read when this manual is built.

## A Personal Note

This suite exists because I wanted to make it.  I enjoy lower-level programming
and like building things that compose well together.  It's written in C simply
because I like C.  I like other languages, too.

I'm releasing the libraries as LGPL-3.0-only.  The libraries are intended to be
cross-platform (Widows and Linux, but hopefully Mac, too, if someone gives me a
Mac), and the headers are usable from C++.

If you want to use parts of this library in some way other than the copyleft
license, then I would consider relicensing under a proprietary license for a
fee.  If you wish to contact me, write to me using my gmail address.  The first
part of it is `pennycuff.c`.

## Compiling Into Your Project

I have tried to make it easy to incorporate into your own programs (otherwise,
what's the point?).  Once a library is installed, a program links what it needs
through `pkg-config`:

```
pkg-config --cflags --libs ghoti.io-text-0
```

The module name is `ghoti.io-<library>-<major>`. The libraries share that
major, so one suffix names the whole suite.

Most of the Ghoti.io libraries only need the C standard library and other
libraries from this suite.  Some libraries, however are more difficult.  A GUI,
for example, requires vendor specialization, and I don't know how to get around
that.  So CJelly, to name one, must link to Vulkan and X11 on Linux.

## Artificial Intelligence

Yes, I used AI to help with development.  And, I wrote huge parts of it myself
before AI invaded programming.  None of it is hands-off.  I have invested in
this project both monitarily and with untold personal hours of work.  It's not
slop, and I will defend that.

AI doesn't create perfect software, but neither do I.  I **will** claim, though,
that the software that I have produced is better when I use AI than if either
one (AI or myself) had done it without the other.

I will say this: one of the pleasures of working with AI is that it will
*consistently* do the things that I might forget because of the tedium.  Asan,
UBsan, Tsan, Fuzzing, Soaking... the AI won't complain, and the project as a
whole is better because of it.
