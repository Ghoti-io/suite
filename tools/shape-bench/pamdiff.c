/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of the Ghoti.io suite's shape benchmark harness.
 *
 * It is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License version 3 as published by
 * the Free Software Foundation.
 *
 * It is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file pamdiff.c
 *
 * The cross-check's measure: two RGBA PAM files of one extent, compared
 * channel by channel.
 *
 *     pamdiff REFERENCE CANDIDATE
 *
 * prints one JSON object: the pixel count, the mean absolute difference over
 * every channel of every pixel (8-bit levels), the largest difference, the
 * pixels with any channel more than PAMDIFF_BAD levels apart and their
 * fraction, and the pixels that differ at all.  Exit 0 when both files were
 * read, 1 otherwise; whether the difference is within the bound is the
 * report's decision (report.py), so that the bound is stated in one place.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAMDIFF_BAD 32

typedef struct Pam {
  int w, h;
  uint8_t * px;
} Pam;

static int read_pam(const char * path, Pam * out) {
  FILE * f = fopen(path, "rb");
  if (!f) {
    return -1;
  }
  char line[128];
  int w = -1, h = -1, depth = -1, maxval = -1, ok = 0;
  if (!fgets(line, sizeof line, f) || strcmp(line, "P7\n") != 0) {
    fclose(f);
    return -1;
  }
  while (fgets(line, sizeof line, f)) {
    if (!strcmp(line, "ENDHDR\n")) {
      ok = 1;
      break;
    }
    sscanf(line, "WIDTH %d", &w);
    sscanf(line, "HEIGHT %d", &h);
    sscanf(line, "DEPTH %d", &depth);
    sscanf(line, "MAXVAL %d", &maxval);
  }
  if (!ok || w <= 0 || h <= 0 || depth != 4 || maxval != 255) {
    fclose(f);
    return -1;
  }
  size_t n = (size_t)w * (size_t)h * 4;
  out->px = malloc(n);
  if (!out->px || fread(out->px, 1, n, f) != n) {
    free(out->px);
    fclose(f);
    return -1;
  }
  fclose(f);
  out->w = w;
  out->h = h;
  return 0;
}

int main(int argc, char ** argv) {
  Pam a = {0, 0, NULL}, b = {0, 0, NULL};
  if (argc != 3 || read_pam(argv[1], &a) || read_pam(argv[2], &b) || a.w != b.w || a.h != b.h) {
    fprintf(stderr, "pamdiff: cannot compare %s and %s\n", argc > 1 ? argv[1] : "?", argc > 2 ? argv[2] : "?");
    return 1;
  }
  size_t pixels = (size_t)a.w * (size_t)a.h, bad = 0, differ = 0;
  uint64_t sum = 0;
  int max = 0;
  for (size_t i = 0; i < pixels; i++) {
    int worst = 0;
    for (int c = 0; c < 4; c++) {
      int d = (int)a.px[4 * i + c] - (int)b.px[4 * i + c];
      d = d < 0 ? -d : d;
      sum += (uint64_t)d;
      worst = d > worst ? d : worst;
    }
    max = worst > max ? worst : max;
    bad += worst > PAMDIFF_BAD;
    differ += worst > 0;
  }
  printf("{\"pixels\":%zu,\"mean_abs\":%.6f,\"max_abs\":%d,\"bad_threshold\":%d,\"bad_pixels\":%zu,"
         "\"bad_fraction\":%.8f,\"differing_pixels\":%zu}\n",
      pixels, (double)sum / (double)(pixels * 4), max, PAMDIFF_BAD, bad, (double)bad / (double)pixels, differ);
  free(a.px);
  free(b.px);
  return 0;
}
