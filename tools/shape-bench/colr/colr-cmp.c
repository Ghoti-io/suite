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
 * @file colr-cmp.c
 *
 * Two atlases of glyph cells (`colr-shape` and `colr-skia`; of one width, the glyph list naming the cells to compare), compared cell by cell with the harness's measure
 * (pamdiff.c): for each cell the mean absolute difference over every channel of every pixel (8-bit levels) and
 * the fraction of its pixels with any channel more than 32 levels apart; and, for the cell's ink (a pixel either
 * engine drew), the same two figures over the ink alone, which the cell's empty area does not dilute.
 *
 * It adds the figure the gate bounds in place of the raw fraction: `tbig`, the fraction of the cell's pixels whose value
 * is more than 32 levels outside the range of the other picture's values within one pixel (each channel; and the same
 * the other way round: a pixel is counted when neither picture's value fits the other's neighbourhood). Two engines that
 * antialias an edge differently put a different value on an edge pixel, and an edge that is a fraction of a pixel apart
 * is not a different picture; a wrong color, a gradient that runs the wrong way or a missing layer is out of range.
 * (Measured on Noto Color Emoji at 128 pixels, 79 % of the glyphs have more than 0.1 % of their cell's pixels 32 levels
 * apart in the raw count, all of it along edges.)
 *
 *     colr-cmp REFERENCE.pam CANDIDATE.pam GLYPHS CELL COLS
 *
 * prints one line a glyph: `<glyph> ink=<pixels> mean=<m> big=<fraction> tbig=<fraction> inkmean=<m> inkbig=<fraction>`.
 * Whether a glyph is within the bound is the report's decision (colr-report.py), so the bound is stated in one place.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  int w = -1, h = -1, ok = 0;
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
  }
  if (!ok || w <= 0 || h <= 0) {
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
  if (argc != 6 && argc != 7) {
    fprintf(stderr, "usage: colr-cmp REFERENCE.pam CANDIDATE.pam GLYPHS CELL COLS [RADIUS]\n");
    return 2;
  }
  Pam a, b;
  if (read_pam(argv[1], &a) || read_pam(argv[2], &b) || a.w != b.w) {
    fprintf(stderr, "colr-cmp: cannot read two atlases of one width\n");
    return 1;
  }
  unsigned cell = (unsigned)atoi(argv[4]);
  unsigned cols = (unsigned)atoi(argv[5]);
  int radius = argc == 7 ? atoi(argv[6]) : 1;
  FILE * glyphs = fopen(argv[3], "r");
  if (!glyphs || cell == 0 || cols == 0 || (unsigned)a.w != cols * cell) {
    fprintf(stderr, "colr-cmp: the glyph list or the layout is wrong\n");
    return 1;
  }
  unsigned g;
  size_t index = 0;
  while (fscanf(glyphs, "%u", &g) == 1) {
    unsigned cx = (unsigned)(index % cols) * cell;
    unsigned cy = (unsigned)(index / cols) * cell;
    if ((int)(cy + cell) > a.h || (int)(cy + cell) > b.h) {
      fprintf(stderr, "colr-cmp: glyph %u is past the end of an atlas\n", g);
      return 1;
    }
    uint64_t sum = 0, ink = 0, ink_sum = 0, big = 0, ink_big = 0, tbig = 0;
    for (unsigned y = 0; y < cell; y++) {
      for (unsigned x = 0; x < cell; x++) {
        const uint8_t * p = a.px + ((size_t)(cy + y) * (size_t)a.w + cx + x) * 4;
        const uint8_t * q = b.px + ((size_t)(cy + y) * (size_t)a.w + cx + x) * 4;
        int worst = 0;
        uint64_t d = 0;
        for (int k = 0; k < 4; k++) {
          int e = abs((int)p[k] - (int)q[k]);
          d += (uint64_t)e;
          worst = e > worst ? e : worst;
        }
        sum += d;
        big += worst > 32;
        if (worst > 32) {
          /* A value an edge moved by up to RADIUS pixels would put here is any in the range of the other picture's
           * neighbourhood (an edge pixel's coverage runs between its two sides); beyond that, 32 levels more. */
          int in_a = 1, in_b = 1;
          for (int k = 0; k < 4; k++) {
            int lo_a = 255, hi_a = 0, lo_b = 255, hi_b = 0;
            for (int dy = -radius; dy <= radius; dy++) {
              for (int dx = -radius; dx <= radius; dx++) {
                int yy = (int)y + dy, xx = (int)x + dx;
                if (yy < 0 || xx < 0 || yy >= (int)cell || xx >= (int)cell) {
                  continue;
                }
                size_t at = ((size_t)(cy + (unsigned)yy) * (size_t)a.w + cx + (unsigned)xx) * 4;
                int va = a.px[at + k], vb = b.px[at + k];
                lo_a = va < lo_a ? va : lo_a;
                hi_a = va > hi_a ? va : hi_a;
                lo_b = vb < lo_b ? vb : lo_b;
                hi_b = vb > hi_b ? vb : hi_b;
              }
            }
            /* b's pixel against a's neighbourhood, and a's against b's. */
            in_a &= (int)q[k] >= lo_a - 32 && (int)q[k] <= hi_a + 32;
            in_b &= (int)p[k] >= lo_b - 32 && (int)p[k] <= hi_b + 32;
          }
          tbig += !(in_a || in_b);
        }
        if (p[3] || q[3]) {
          ink++;
          ink_sum += d;
          ink_big += worst > 32;
        }
      }
    }
    double pixels = (double)cell * (double)cell;
    printf("%u ink=%llu mean=%.4f big=%.6f tbig=%.6f inkmean=%.4f inkbig=%.6f\n", g, (unsigned long long)ink,
        (double)sum / (4.0 * pixels), (double)big / pixels, (double)tbig / pixels, ink ? (double)ink_sum / (4.0 * (double)ink) : 0.0,
        ink ? (double)ink_big / (double)ink : 0.0);
    index++;
  }
  return 0;
}
