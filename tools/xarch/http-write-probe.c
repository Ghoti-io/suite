/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io HTTP.
 *
 * Ghoti.io HTTP is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 */

/*
 * The writer and the parser, round trip, for suite/tools/xarch/http.sh and
 * suite/tools/xwin/http.sh.
 *
 * Builds a request and a response with the public API, writes each with a
 * chunked body (one chunk larger than the buffer's first allocations, so the
 * growth path runs), prints the bytes written as hex and an FNV-1a hash of
 * them, parses them back whole and a byte at a time, and prints what came
 * out. The output is a pure function of the library's behaviour, so every
 * target must print the same text; the script compares it byte for byte with
 * the host's. It exits non-zero if the parse of what was written is not what
 * was written.
 */

#include <ghoti.io/http/http.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int failures;

static void check(int ok, const char * what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    failures++;
  }
}

static uint32_t fnv1a(const unsigned char * p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ p[i]) * 16777619u;
  }
  return h;
}

static void hex(const unsigned char * p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    printf("%02x", p[i]);
  }
}

// Parse all of data, in pieces of `step` bytes (0 is all at once), and print
// the messages. Returns the body it read.
static size_t reparse(GHTTP_ParserKind kind, const unsigned char * data,
    size_t len, size_t step, unsigned char ** body_out) {
  GHTTP_Parser * p = NULL;
  check(ghttp_parser_create(kind, NULL, NULL, &p) == GHTTP_OK, "parser_create");
  unsigned char * body = malloc(len + 1);
  size_t body_len = 0;
  size_t off = 0;
  int ends = 0;
  while (off < len) {
    size_t piece = step ? (len - off < step ? len - off : step) : len - off;
    size_t at = 0;
    GHTTP_Event ev;
    do {
      size_t used = 0;
      GHTTP_Result r = ghttp_parser_feed(p, data + off + at, piece - at, &used, &ev);
      if (r != GHTTP_OK) {
        printf("FAIL reparse: %s at %zu\n", ghttp_result_string(r), off + at);
        failures++;
        goto done;
      }
      at += used;
      if (ev == GHTTP_EVENT_BODY) {
        const void * d;
        size_t n;
        ghttp_parser_body(p, &d, &n);
        memcpy(body + body_len, d, n);
        body_len += n;
      } else if (ev == GHTTP_EVENT_END) {
        ends++;
      }
    } while (ev != GHTTP_EVENT_NEED_MORE);
    off += piece;
  }
done:
  check(ends == 1, "one message ended");
  printf("  reparsed step=%zu body=%zu bytes hash=%08x trailers=%zu\n", step,
      body_len, fnv1a(body, body_len),
      ghttp_headers_count(ghttp_parser_trailers(p)));
  ghttp_parser_destroy(p);
  *body_out = body;
  return body_len;
}

static void roundtrip(int is_response) {
  GHTTP_Buffer * out = NULL;
  check(ghttp_buffer_create(NULL, &out) == GHTTP_OK, "buffer_create");
  GHTTP_Headers * trailers = NULL;
  check(ghttp_headers_create(NULL, &trailers) == GHTTP_OK, "headers_create");
  check(ghttp_headers_add(trailers, "X-Digest", "abc123") == GHTTP_OK, "trailer");

  if (!is_response) {
    GHTTP_Request * q = NULL;
    check(ghttp_request_create(NULL, &q) == GHTTP_OK, "request_create");
    check(ghttp_request_set_method(q, "POST") == GHTTP_OK, "method");
    check(ghttp_request_set_target(q, "/upload?x=1") == GHTTP_OK, "target");
    check(ghttp_headers_add(ghttp_request_headers(q), "Host", "example.test") == GHTTP_OK, "host");
    check(ghttp_headers_add(ghttp_request_headers(q), "Transfer-Encoding", "chunked") == GHTTP_OK, "te");
    check(ghttp_write_request_head(out, q) == GHTTP_OK, "write head");
    ghttp_request_destroy(q);
  } else {
    GHTTP_Response * q = NULL;
    check(ghttp_response_create(NULL, &q) == GHTTP_OK, "response_create");
    check(ghttp_response_set_status(q, 200, "OK") == GHTTP_OK, "status");
    check(ghttp_headers_add(ghttp_response_headers(q), "Transfer-Encoding", "chunked") == GHTTP_OK, "te");
    check(ghttp_write_response_head(out, q) == GHTTP_OK, "write head");
    ghttp_response_destroy(q);
  }

  // A body that is not a multiple of anything: every byte value, 70001 of
  // them, in three chunks, the middle one well past the first growth steps.
  const size_t sizes[3] = {5, 70001, 3};
  unsigned char * body = malloc(70009);
  size_t total = 0;
  for (int c = 0; c < 3; c++) {
    for (size_t i = 0; i < sizes[c]; i++) {
      body[total + i] = (unsigned char)((total + i) * 7 + c);
    }
    check(ghttp_write_chunk(out, body + total, sizes[c]) == GHTTP_OK, "write chunk");
    total += sizes[c];
  }
  check(ghttp_write_chunked_end(out, trailers) == GHTTP_OK, "write end");

  const unsigned char * wire = ghttp_buffer_data(out);
  size_t wire_len = ghttp_buffer_size(out);
  printf("%s: wrote %zu bytes, hash=%08x\n", is_response ? "response" : "request",
      wire_len, fnv1a(wire, wire_len));
  printf("  head: ");
  hex(wire, 80 < wire_len ? 80 : wire_len);
  printf("\n");

  GHTTP_ParserKind kind = is_response ? GHTTP_PARSE_RESPONSE : GHTTP_PARSE_REQUEST;
  const size_t steps[4] = {0, 1, 3, 4097};
  for (int s = 0; s < 4; s++) {
    unsigned char * got = NULL;
    size_t n = reparse(kind, wire, wire_len, steps[s], &got);
    check(n == total && memcmp(got, body, total) == 0, "the body read is the body written");
    free(got);
  }
  free(body);
  ghttp_headers_destroy(trailers);
  ghttp_buffer_destroy(out);
}

int main(void) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY); // compared byte for byte with another platform's
#endif
  printf("sizeof size_t=%zu\n", sizeof(size_t));
  roundtrip(0);
  roundtrip(1);
  printf(failures ? "FAILED %d\n" : "ok\n", failures);
  return failures ? 1 : 0;
}
