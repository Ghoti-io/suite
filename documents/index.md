# Ghoti.io (it's pronounced "[Fish](https://en.wikipedia.org/wiki/Ghoti)")

This is a suite of cross-platform C libraries.

There are also puns.  Please forgive me.

Clone the libraries, build them, and build this manual from
[Getting Started](@ref getting_started).

## Libraries

| Library | What it implements |
| --- | --- |
| [Archive](@ref archive) | Tar from a file or a pipe: v7, ustar, GNU, and pax, with member names classified rather than rewritten. Zip is not implemented yet. |
| [Chron](@ref chron) | Civil time, calendars, instants, durations, time zones, and the text formats for them. |
| [CJelly](@ref cjelly) | A Vulkan GUI: native windows, and a renderer for panels, an image, and a Wavefront model. |
| [Color](@ref color) | A colour engine's scaffold: the result vocabulary, the allocator alias, and the version accessors. |
| [Compress](@ref compress) | Deflate, zlib, gzip, LZ4, LZW, RLE, and zstd. |
| [CTang](@ref ctang) | A template language for a host program, with an x86-64 JIT and a bytecode fallback. |
| [CUtil](@ref cutil) | An allocator, containers, checked arithmetic, threads, and the filesystem. |
| [Font](@ref font) | An sfnt reader: the container, metric tables, `cmap`, and names. |
| [Image](@ref image) | PNG, including APNG, and JPEG, BMP, and GIF. |
| [Model](@ref model) | Wavefront OBJ and MTL. |
| [Regex](@ref regex) | Regular expressions. Which of the seventeen named dialects compile is measured with the rest of the figures below. |
| [Security](@ref security) | SHA-256, SHA-512, SHA-384, SHA-1, and MD5, with HMAC, HKDF, PBKDF2, AES (one block, CTR, and GCM), ChaCha20-Poly1305, X25519, Ed25519, P-256 ECDH and ECDSA, and RSA signature verification. |
| [Text](@ref text) | JSON, CSV, and YAML, including JSON Schema. |
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
