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

#define _POSIX_C_SOURCE 200809L

#include "scene.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void sb_scene_init(SB_Scene * scene) {
  memset(scene, 0, sizeof *scene);
}

void sb_scene_free(SB_Scene * scene) {
  free(scene->items);
  free(scene->cmds);
  free(scene->glyphs);
  free(scene->gradients);
  free(scene->stops);
  free(scene->strokes);
  sb_scene_init(scene);
}

static int grow(void ** data, size_t * cap, size_t need, size_t size) {
  if (need <= *cap) {
    return 0;
  }
  size_t n = *cap ? *cap * 2 : 64;
  while (n < need) {
    n *= 2;
  }
  void * p = realloc(*data, n * size);
  if (!p) {
    return -1;
  }
  *data = p;
  *cap = n;
  return 0;
}

int sb_scene_add_item(SB_Scene * scene, const SB_Item * item) {
  if (grow((void **)&scene->items, &scene->cap_items, scene->n_items + 1, sizeof *item)) {
    return -1;
  }
  scene->items[scene->n_items++] = *item;
  return 0;
}

int sb_scene_add_cmd(SB_Scene * scene, char op, const double * p) {
  if (grow((void **)&scene->cmds, &scene->cap_cmds, scene->n_cmds + 1, sizeof(SB_Cmd))) {
    return -1;
  }
  SB_Cmd * c = &scene->cmds[scene->n_cmds++];
  memset(c, 0, sizeof *c);
  c->op = op;
  int n = op == 'M' || op == 'L' ? 2 : op == 'Q' ? 4 : op == 'C' ? 6 : 0;
  for (int i = 0; i < n; i++) {
    c->p[i] = p[i];
  }
  return 0;
}

int sb_scene_add_glyph(SB_Scene * scene, uint32_t id, double x, double y) {
  if (grow((void **)&scene->glyphs, &scene->cap_glyphs, scene->n_glyphs + 1, sizeof(SB_Glyph))) {
    return -1;
  }
  SB_Glyph * g = &scene->glyphs[scene->n_glyphs++];
  g->id = id;
  g->x = x;
  g->y = y;
  return 0;
}

int sb_scene_add_gradient(SB_Scene * scene, const SB_Gradient * gradient) {
  if (grow((void **)&scene->gradients, &scene->cap_gradients, scene->n_gradients + 1, sizeof *gradient)) {
    return -1;
  }
  scene->gradients[scene->n_gradients++] = *gradient;
  return 0;
}

int sb_scene_add_stop(SB_Scene * scene, double offset, SB_Color color) {
  if (grow((void **)&scene->stops, &scene->cap_stops, scene->n_stops + 1, sizeof(SB_Stop))) {
    return -1;
  }
  SB_Stop * s = &scene->stops[scene->n_stops++];
  s->offset = offset;
  s->color = color;
  return 0;
}

int sb_scene_add_stroke(SB_Scene * scene, const SB_Stroke * stroke) {
  if (grow((void **)&scene->strokes, &scene->cap_strokes, scene->n_strokes + 1, sizeof *stroke)) {
    return -1;
  }
  scene->strokes[scene->n_strokes++] = *stroke;
  return 0;
}

static const char * const JOIN_NAMES[] = {"miter", "round", "bevel", "miterclip", "arcs"};
#define N_JOINS 5

static const char * const COMPOSITE_NAMES[SB_COMPOSITE_COUNT] = {"srcover", "clear", "src", "dest", "destover", "srcin",
    "destin", "srcout", "destout", "srcatop", "destatop", "xor", "plus", "screen", "overlay", "darken", "lighten",
    "colordodge", "colorburn", "hardlight", "softlight", "difference", "exclusion", "multiply", "hue", "saturation",
    "color", "luminosity"};

const char * sb_composite_name(SB_Composite op) {
  return (unsigned)op < SB_COMPOSITE_COUNT ? COMPOSITE_NAMES[op] : "srcover";
}
static const char * const CAP_NAMES[] = {"butt", "round", "square"};

static const char * const KIND_NAMES[] = {"", "linear", "radial", "sweep"};
static const char * const EXTEND_NAMES[] = {"pad", "repeat", "reflect"};

/** The number of geometry values a gradient kind carries. */
static int gradient_arity(SB_Gradient_Kind kind) {
  return kind == SB_GRADIENT_LINEAR ? 4 : kind == SB_GRADIENT_RADIAL ? 6 : 4;
}

size_t sb_scene_drawables(const SB_Scene * scene) {
  size_t n = 0;
  for (size_t i = 0; i < scene->n_items; i++) {
    n += scene->items[i].kind == SB_FILL || scene->items[i].kind == SB_RUN || scene->items[i].kind == SB_STROKE;
  }
  return n;
}

static int cmd_arity(char op) {
  switch (op) {
  case 'M':
  case 'L':
    return 2;
  case 'Q':
    return 4;
  case 'C':
    return 6;
  case 'Z':
    return 0;
  default:
    return -1;
  }
}

static int fail(char * err, size_t len, int line, const char * what) {
  if (err && len) {
    snprintf(err, len, "line %d: %s", line, what);
  }
  return -1;
}

static int read_color(const char * s, SB_Color * c, unsigned * count) {
  unsigned r, g, b, a;
  if (sscanf(s, "%u %u %u %u %u", &r, &g, &b, &a, count) != 5 || r > 255 || g > 255 || b > 255 || a > 255) {
    return -1;
  }
  c->r = (uint8_t)r;
  c->g = (uint8_t)g;
  c->b = (uint8_t)b;
  c->a = (uint8_t)a;
  return 0;
}

int sb_scene_load(const char * path, SB_Scene * out, char * err, size_t errlen) {
  FILE * f = fopen(path, "r");
  if (!f) {
    return fail(err, errlen, 0, "cannot open");
  }
  SB_Scene s;
  sb_scene_init(&s);
  char buf[512];
  int line = 0, header = 0, ended = 0;
  int status = 0;
  size_t pending = 0; /* 1 + the gradient the next record, a fill, paints with */
  while (fgets(buf, sizeof buf, f)) {
    line++;
    char * nl = strchr(buf, '\n');
    if (!nl) {
      status = fail(err, errlen, line, "line too long");
      break;
    }
    *nl = 0;
    if (!header) {
      if (strcmp(buf, "shape-bench-scene 1") != 0) {
        status = fail(err, errlen, line, "not a version 1 scene");
        break;
      }
      header = 1;
      continue;
    }
    if (ended) {
      status = fail(err, errlen, line, "text after end");
      break;
    }
    char word[16] = {0};
    int used = 0;
    if (sscanf(buf, "%15s%n", word, &used) != 1) {
      status = fail(err, errlen, line, "empty line");
      break;
    }
    const char * rest = buf + used;
    if (pending && strcmp(word, "fill") != 0 && strcmp(word, "fill-eo") != 0 && strcmp(word, "stroke") != 0) {
      status = fail(err, errlen, line, "a gradient not followed by a fill or a stroke");
      break;
    }
    if (!strcmp(word, "name")) {
      if (sscanf(rest, " %63s", s.name) != 1) {
        status = fail(err, errlen, line, "bad name");
        break;
      }
    } else if (!strcmp(word, "metric")) {
      if (sscanf(rest, " %15s", s.metric) != 1) {
        status = fail(err, errlen, line, "bad metric");
        break;
      }
    } else if (!strcmp(word, "extent")) {
      if (sscanf(rest, "%d %d", &s.width, &s.height) != 2 || s.width <= 0 || s.height <= 0 || s.width > 16384 ||
          s.height > 16384) {
        status = fail(err, errlen, line, "bad extent");
        break;
      }
    } else if (!strcmp(word, "background")) {
      unsigned r, g, b, a;
      if (sscanf(rest, "%u %u %u %u", &r, &g, &b, &a) != 4 || r > 255 || g > 255 || b > 255 || a > 255) {
        status = fail(err, errlen, line, "bad background");
        break;
      }
      s.background.r = (uint8_t)r;
      s.background.g = (uint8_t)g;
      s.background.b = (uint8_t)b;
      s.background.a = (uint8_t)a;
    } else if (!strcmp(word, "work")) {
      if (sscanf(rest, " %15s %lf", s.work_unit, &s.work) != 2) {
        status = fail(err, errlen, line, "bad work");
        break;
      }
    } else if (!strcmp(word, "font")) {
      if (sscanf(rest, " %255s %lf", s.font, &s.font_size) != 2 || s.font_size <= 0) {
        status = fail(err, errlen, line, "bad font");
        break;
      }
    } else if (!strcmp(word, "clip")) {
      SB_Item it;
      memset(&it, 0, sizeof it);
      it.kind = SB_CLIP;
      if (sscanf(rest, "%lf %lf %lf %lf", &it.rect[0], &it.rect[1], &it.rect[2], &it.rect[3]) != 4 ||
          sb_scene_add_item(&s, &it)) {
        status = fail(err, errlen, line, "bad clip");
        break;
      }
    } else if (!strcmp(word, "unclip")) {
      SB_Item it;
      memset(&it, 0, sizeof it);
      it.kind = SB_UNCLIP;
      if (sb_scene_add_item(&s, &it)) {
        status = fail(err, errlen, line, "out of memory");
        break;
      }
    } else if (!strcmp(word, "group")) {
      SB_Item it;
      memset(&it, 0, sizeof it);
      it.kind = SB_GROUP;
      char name[32] = {0}, flag[16] = {0};
      int op = -1;
      int got = sscanf(rest, " %31s %lf %15s", name, &it.opacity, flag);
      it.linear = got == 3 && !strcmp(flag, "linear");
      if (got < 2 || (got == 3 && !it.linear) || !(it.opacity >= 0 && it.opacity <= 1)) {
        status = fail(err, errlen, line, "bad group");
        break;
      }
      for (int k = 0; k < SB_COMPOSITE_COUNT; k++) {
        op = !strcmp(name, COMPOSITE_NAMES[k]) ? k : op;
      }
      it.composite = op;
      if (op < 0 || sb_scene_add_item(&s, &it)) {
        status = fail(err, errlen, line, "bad group");
        break;
      }
    } else if (!strcmp(word, "endgroup")) {
      SB_Item it;
      memset(&it, 0, sizeof it);
      it.kind = SB_ENDGROUP;
      if (sb_scene_add_item(&s, &it)) {
        status = fail(err, errlen, line, "out of memory");
        break;
      }
    } else if (!strcmp(word, "clippath")) {
      SB_Item it;
      memset(&it, 0, sizeof it);
      it.kind = SB_CLIPPATH;
      unsigned long count = 0;
      if (sscanf(rest, " %lu", &count) != 1 || count == 0) {
        status = fail(err, errlen, line, "bad clippath");
        break;
      }
      it.first = s.n_cmds;
      it.count = count;
      for (unsigned long k = 0; k < count && status == 0; k++) {
        if (!fgets(buf, sizeof buf, f) || !strchr(buf, '\n')) {
          status = fail(err, errlen, line + 1, "truncated item");
          break;
        }
        line++;
        double p[6] = {0};
        char op = buf[0];
        int arity = cmd_arity(op);
        int got = arity <= 0 ? 0 : sscanf(buf + 1, "%lf %lf %lf %lf %lf %lf", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]);
        if (arity < 0 || got != arity || (k == 0 && op != 'M') || sb_scene_add_cmd(&s, op, p)) {
          status = fail(err, errlen, line, "bad path command");
        }
      }
      if (status == 0 && sb_scene_add_item(&s, &it)) {
        status = fail(err, errlen, line, "out of memory");
      }
      if (status) {
        break;
      }
    } else if (!strcmp(word, "fill") || !strcmp(word, "fill-eo") || !strcmp(word, "run")) {
      SB_Item it;
      memset(&it, 0, sizeof it);
      unsigned count = 0;
      it.kind = word[0] == 'f' ? SB_FILL : SB_RUN;
      it.evenodd = word[4] == '-';
      if (read_color(rest, &it.color, &count) || count == 0) {
        status = fail(err, errlen, line, "bad fill or run");
        break;
      }
      it.first = it.kind == SB_FILL ? s.n_cmds : s.n_glyphs;
      it.count = count;
      if (it.kind == SB_FILL && pending) {
        if (it.color.r || it.color.g || it.color.b) {
          status = fail(err, errlen, line, "a gradient fill's color is not 0 0 0 a");
          break;
        }
        it.gradient = pending;
        pending = 0;
      }
      for (unsigned k = 0; k < count && status == 0; k++) {
        if (!fgets(buf, sizeof buf, f) || !strchr(buf, '\n')) {
          status = fail(err, errlen, line + 1, "truncated item");
          break;
        }
        line++;
        if (it.kind == SB_FILL) {
          double p[6] = {0};
          char op = buf[0];
          int arity = cmd_arity(op);
          int got = arity <= 0 ? 0 : sscanf(buf + 1, "%lf %lf %lf %lf %lf %lf", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]);
          if (arity < 0 || got != arity || (k == 0 && op != 'M') || sb_scene_add_cmd(&s, op, p)) {
            status = fail(err, errlen, line, "bad path command");
          }
        } else {
          unsigned id;
          double x, y;
          if (sscanf(buf, "g %u %lf %lf", &id, &x, &y) != 3 || sb_scene_add_glyph(&s, id, x, y)) {
            status = fail(err, errlen, line, "bad glyph");
          }
        }
      }
      if (status == 0 && sb_scene_add_item(&s, &it)) {
        status = fail(err, errlen, line, "out of memory");
      }
      if (status) {
        break;
      }
    } else if (!strcmp(word, "stroke")) {
      SB_Item it;
      SB_Stroke st;
      memset(&it, 0, sizeof it);
      memset(&st, 0, sizeof st);
      it.kind = SB_STROKE;
      unsigned r, g, b, a;
      char join[16] = {0}, cap[16] = {0};
      int used = 0;
      unsigned long ndash = 0;
      if (sscanf(rest, " %u %u %u %u %lf %15s %15s %lf %lf %lu%n", &r, &g, &b, &a, &st.width, join, cap, &st.miter,
              &st.offset, &ndash, &used) != 10 ||
          r > 255 || g > 255 || b > 255 || a > 255 || !(st.width > 0) || !(st.miter >= 1) || ndash > SB_DASHES_MAX ||
          (ndash % 2)) {
        status = fail(err, errlen, line, "bad stroke");
        break;
      }
      int j = -1, cp = -1;
      for (int k = 0; k < N_JOINS; k++) {
        j = !strcmp(join, JOIN_NAMES[k]) ? k : j;
      }
      for (int k = 0; k < 3; k++) {
        cp = !strcmp(cap, CAP_NAMES[k]) ? k : cp;
      }
      const char * at = rest + used;
      for (unsigned long k = 0; k < ndash && j >= 0 && cp >= 0; k++) {
        char * end = NULL;
        st.dashes[k] = strtod(at, &end);
        if (end == at || !(st.dashes[k] >= 0)) {
          j = -1;
        }
        at = end;
      }
      char * end = NULL;
      unsigned long count = j < 0 || cp < 0 ? 0 : strtoul(at, &end, 10);
      if (j < 0 || cp < 0 || count == 0 || end == at) {
        status = fail(err, errlen, line, "bad stroke");
        break;
      }
      st.join = (SB_Join)j;
      st.cap = (SB_Cap)cp;
      st.n_dashes = ndash;
      it.color.r = (uint8_t)r;
      it.color.g = (uint8_t)g;
      it.color.b = (uint8_t)b;
      it.color.a = (uint8_t)a;
      it.first = s.n_cmds;
      it.count = count;
      if (pending) {
        if (r || g || b) {
          status = fail(err, errlen, line, "a gradient stroke's color is not 0 0 0 a");
          break;
        }
        it.gradient = pending;
        pending = 0;
      }
      for (unsigned long k = 0; k < count && status == 0; k++) {
        if (!fgets(buf, sizeof buf, f) || !strchr(buf, '\n')) {
          status = fail(err, errlen, line + 1, "truncated item");
          break;
        }
        line++;
        double p[6] = {0};
        char op = buf[0];
        int arity = cmd_arity(op);
        int got = arity <= 0 ? 0 : sscanf(buf + 1, "%lf %lf %lf %lf %lf %lf", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]);
        if (arity < 0 || got != arity || (k == 0 && op != 'M') || sb_scene_add_cmd(&s, op, p)) {
          status = fail(err, errlen, line, "bad path command");
        }
      }
      if (status == 0 && (sb_scene_add_stroke(&s, &st) || ((it.stroke = s.n_strokes), sb_scene_add_item(&s, &it)))) {
        status = fail(err, errlen, line, "out of memory");
      }
      if (status) {
        break;
      }
    } else if (!strcmp(word, "gradient")) {
      SB_Gradient g;
      memset(&g, 0, sizeof g);
      char kind[16] = {0}, extend[16] = {0};
      int used2 = 0;
      double v[7];
      if (sscanf(rest, " %15s %15s%n", kind, extend, &used2) != 2) {
        status = fail(err, errlen, line, "bad gradient");
        break;
      }
      for (int k = 1; k <= 3; k++) {
        g.kind = !strcmp(kind, KIND_NAMES[k]) ? (SB_Gradient_Kind)k : g.kind;
      }
      int e = -1;
      for (int k = 0; k < 3; k++) {
        e = !strcmp(extend, EXTEND_NAMES[k]) ? k : e;
      }
      int arity = g.kind ? gradient_arity(g.kind) : 0;
      int got = sscanf(rest + used2, "%lf %lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
      if (!g.kind || e < 0 || got != arity + 1 || v[arity] != floor(v[arity]) || v[arity] < 2 ||
          v[arity] > SB_GRADIENT_STOPS_MAX) {
        status = fail(err, errlen, line, "bad gradient");
        break;
      }
      for (int k = 0; k < arity; k++) {
        if (!isfinite(v[k])) {
          status = fail(err, errlen, line, "bad gradient geometry");
        }
        g.g[k] = v[k];
      }
      g.extend = (SB_Extend)e;
      g.first_stop = s.n_stops;
      g.n_stops = (size_t)v[arity];
      for (size_t k = 0; k < g.n_stops && status == 0; k++) {
        double offset;
        unsigned r, gg, b, a;
        if (!fgets(buf, sizeof buf, f) || !strchr(buf, '\n')) {
          status = fail(err, errlen, line + 1, "truncated gradient");
          break;
        }
        line++;
        if (sscanf(buf, "s %lf %u %u %u %u", &offset, &r, &gg, &b, &a) != 5 || r > 255 || gg > 255 || b > 255 ||
            a > 255 || !(offset >= 0 && offset <= 1) || (k == 0 && offset != 0) ||
            (k + 1 == g.n_stops && offset != 1) || (k > 0 && offset < s.stops[s.n_stops - 1].offset)) {
          status = fail(err, errlen, line, "bad stop");
          break;
        }
        SB_Color c = {(uint8_t)r, (uint8_t)gg, (uint8_t)b, (uint8_t)a};
        if (sb_scene_add_stop(&s, offset, c)) {
          status = fail(err, errlen, line, "out of memory");
        }
      }
      if (status == 0 && sb_scene_add_gradient(&s, &g)) {
        status = fail(err, errlen, line, "out of memory");
      }
      if (status) {
        break;
      }
      pending = s.n_gradients;
    } else if (!strcmp(word, "end")) {
      ended = 1;
    } else {
      status = fail(err, errlen, line, "unknown record");
      break;
    }
  }
  fclose(f);
  if (status == 0 && pending) {
    status = fail(err, errlen, line, "a gradient not followed by a fill or a stroke");
  }
  if (status == 0 && (!ended ||!s.name[0] || !s.metric[0] || !s.width || !s.work_unit[0])) {
    status = fail(err, errlen, line, "incomplete scene");
  }
  if (status == 0 && !strcmp(s.metric, "text") && !s.font[0]) {
    status = fail(err, errlen, line, "a text scene names no font");
  }
  /* Clips and groups nest well: each end closes the innermost open one, of its own kind. */
  char open[256];
  size_t depth = 0;
  for (size_t i = 0; status == 0 && i < s.n_items; i++) {
    SB_Kind k = s.items[i].kind;
    if (k == SB_CLIP || k == SB_CLIPPATH || k == SB_GROUP) {
      if (depth == sizeof open) {
        status = fail(err, errlen, line, "clips and groups nest too deep");
      } else {
        open[depth++] = k == SB_GROUP ? 'g' : 'c';
      }
    } else if (k == SB_UNCLIP || k == SB_ENDGROUP) {
      if (depth == 0 || open[depth - 1] != (k == SB_ENDGROUP ? 'g' : 'c')) {
        status = fail(err, errlen, line, k == SB_ENDGROUP ? "endgroup without group" : "unclip without clip");
      } else {
        depth--;
      }
    }
  }
  if (status == 0 && depth != 0) {
    status = fail(err, errlen, line, "clip or group left open");
  }
  if (status) {
    sb_scene_free(&s);
    return -1;
  }
  *out = s;
  return 0;
}

/* Coordinates are written with every digit a double has (%.17g), so that what is read is what was written: a multiple of 1/64
 * is exact, and so is a picture's curve, whose exact coincidences (the overlapping segments of a closed curve) a rounded
 * coordinate would turn into thousands of tiny crossings. */
int sb_scene_save(const char * path, const SB_Scene * s) {
  FILE * f = fopen(path, "w");
  if (!f) {
    return -1;
  }
  fprintf(f, "shape-bench-scene 1\nname %s\nmetric %s\nextent %d %d\nbackground %u %u %u %u\nwork %s %.6f\n", s->name,
      s->metric, s->width, s->height, s->background.r, s->background.g, s->background.b, s->background.a, s->work_unit,
      s->work);
  if (s->font[0]) {
    fprintf(f, "font %s %.6f\n", s->font, s->font_size);
  }
  for (size_t i = 0; i < s->n_items; i++) {
    const SB_Item * it = &s->items[i];
    const SB_Color * c = &it->color;
    switch (it->kind) {
    case SB_CLIP:
      fprintf(f, "clip %.6f %.6f %.6f %.6f\n", it->rect[0], it->rect[1], it->rect[2], it->rect[3]);
      break;
    case SB_UNCLIP:
      fprintf(f, "unclip\n");
      break;
    case SB_GROUP:
      fprintf(f, "group %s %.9g%s\n", COMPOSITE_NAMES[it->composite], it->opacity, it->linear ? " linear" : "");
      break;
    case SB_ENDGROUP:
      fprintf(f, "endgroup\n");
      break;
    case SB_CLIPPATH:
      fprintf(f, "clippath %zu\n", it->count);
      for (size_t k = 0; k < it->count; k++) {
        const SB_Cmd * m = &s->cmds[it->first + k];
        int n = cmd_arity(m->op);
        fputc(m->op, f);
        for (int j = 0; j < n; j++) {
          fprintf(f, " %.17g", m->p[j]);
        }
        fputc('\n', f);
      }
      break;
    case SB_FILL:
      if (it->gradient) {
        const SB_Gradient * g = &s->gradients[it->gradient - 1];
        fprintf(f, "gradient %s %s", KIND_NAMES[g->kind], EXTEND_NAMES[g->extend]);
        for (int j = 0; j < gradient_arity(g->kind); j++) {
          fprintf(f, " %.17g", g->g[j]);
        }
        fprintf(f, " %zu\n", g->n_stops);
        for (size_t k = 0; k < g->n_stops; k++) {
          const SB_Stop * st = &s->stops[g->first_stop + k];
          fprintf(f, "s %.6f %u %u %u %u\n", st->offset, st->color.r, st->color.g, st->color.b, st->color.a);
        }
      }
      fprintf(f, "%s %u %u %u %u %zu\n", it->evenodd ? "fill-eo" : "fill", c->r, c->g, c->b, c->a, it->count);
      for (size_t k = 0; k < it->count; k++) {
        const SB_Cmd * m = &s->cmds[it->first + k];
        int n = cmd_arity(m->op);
        fputc(m->op, f);
        for (int j = 0; j < n; j++) {
          fprintf(f, " %.17g", m->p[j]);
        }
        fputc('\n', f);
      }
      break;
    case SB_STROKE: {
      const SB_Stroke * st = &s->strokes[it->stroke - 1];
      if (it->gradient) {
        const SB_Gradient * g = &s->gradients[it->gradient - 1];
        fprintf(f, "gradient %s %s", KIND_NAMES[g->kind], EXTEND_NAMES[g->extend]);
        for (int j = 0; j < gradient_arity(g->kind); j++) {
          fprintf(f, " %.17g", g->g[j]);
        }
        fprintf(f, " %zu\n", g->n_stops);
        for (size_t k = 0; k < g->n_stops; k++) {
          const SB_Stop * sp = &s->stops[g->first_stop + k];
          fprintf(f, "s %.6f %u %u %u %u\n", sp->offset, sp->color.r, sp->color.g, sp->color.b, sp->color.a);
        }
      }
      fprintf(f, "stroke %u %u %u %u %.6f %s %s %.6f %.6f %zu", c->r, c->g, c->b, c->a, st->width, JOIN_NAMES[st->join],
          CAP_NAMES[st->cap], st->miter, st->offset, st->n_dashes);
      for (size_t k = 0; k < st->n_dashes; k++) {
        fprintf(f, " %.6f", st->dashes[k]);
      }
      fprintf(f, " %zu\n", it->count);
      for (size_t k = 0; k < it->count; k++) {
        const SB_Cmd * m = &s->cmds[it->first + k];
        int n = cmd_arity(m->op);
        fputc(m->op, f);
        for (int j = 0; j < n; j++) {
          fprintf(f, " %.17g", m->p[j]);
        }
        fputc('\n', f);
      }
      break;
    }
    case SB_RUN:
      fprintf(f, "run %u %u %u %u %zu\n", c->r, c->g, c->b, c->a, it->count);
      for (size_t k = 0; k < it->count; k++) {
        const SB_Glyph * g = &s->glyphs[it->first + k];
        fprintf(f, "g %u %.6f %.6f\n", g->id, g->x, g->y);
      }
      break;
    }
  }
  fprintf(f, "end\n");
  return fclose(f) == 0 ? 0 : -1;
}

double sb_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void * a, const void * b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

static double quantile(const double * sorted, int n, double q) {
  double pos = q * (double)(n - 1);
  int i = (int)pos;
  if (i >= n - 1) {
    return sorted[n - 1];
  }
  double frac = pos - (double)i;
  return sorted[i] + (sorted[i + 1] - sorted[i]) * frac;
}

void sb_stats(double * samples, int n, SB_Stats * out) {
  qsort(samples, (size_t)n, sizeof *samples, cmp_double);
  out->min = samples[0];
  out->max = samples[n - 1];
  out->median = quantile(samples, n, 0.5);
  out->p25 = quantile(samples, n, 0.25);
  out->p75 = quantile(samples, n, 0.75);
}

int sb_measure(const SB_Engine * e, SB_Measure * out) {
  double build[SB_TIMED_FRAMES], submit[SB_TIMED_FRAMES], frame[SB_TIMED_FRAMES];
  int rc;
  memset(out, 0, sizeof *out);
  out->has_build = e->build != NULL;
  double start = sb_now();
  int warm = 0;
  while (warm < SB_WARMUP_FRAMES || sb_now() - start < SB_WARMUP_SECONDS) {
    if ((rc = e->clear(e->ctx)) || (e->build && (rc = e->build(e->ctx))) || (rc = e->submit(e->ctx))) {
      return rc;
    }
    warm++;
  }
  for (int i = 0; i < SB_TIMED_FRAMES; i++) {
    if ((rc = e->clear(e->ctx))) {
      return rc;
    }
    double t0 = sb_now();
    if (e->build && (rc = e->build(e->ctx))) {
      return rc;
    }
    double t1 = sb_now();
    if ((rc = e->submit(e->ctx))) {
      return rc;
    }
    double t2 = sb_now();
    build[i] = t1 - t0;
    submit[i] = t2 - t1;
    frame[i] = t2 - t0;
  }
  out->warmup_frames = warm;
  out->frames = SB_TIMED_FRAMES;
  sb_stats(build, SB_TIMED_FRAMES, &out->build);
  sb_stats(submit, SB_TIMED_FRAMES, &out->submit);
  sb_stats(frame, SB_TIMED_FRAMES, &out->frame);
  return 0;
}

static void stats_json(FILE * out, const char * key, const SB_Stats * s) {
  fprintf(out, "\"%s\":{\"median\":%.9g,\"p25\":%.9g,\"p75\":%.9g,\"min\":%.9g,\"max\":%.9g}", key, s->median, s->p25,
      s->p75, s->min, s->max);
}

void sb_report(FILE * out, const char * engine, const SB_Scene * scene, const SB_Measure * m, const char * output,
    const char * note) {
  fprintf(out,
      "{\"engine\":\"%s\",\"scene\":\"%s\",\"metric\":\"%s\",\"extent\":[%d,%d],\"work_unit\":\"%s\",\"work\":%.9g,"
      "\"warmup_frames\":%d,\"frames\":%d,",
      engine, scene->name, scene->metric, scene->width, scene->height, scene->work_unit, scene->work, m->warmup_frames,
      m->frames);
  if (m->has_build) {
    stats_json(out, "build", &m->build);
  } else {
    fprintf(out, "\"build\":null");
  }
  fputc(',', out);
  stats_json(out, "submit", &m->submit);
  fputc(',', out);
  stats_json(out, "frame", &m->frame);
  fprintf(out, ",\"output\":\"%s\",\"note\":", output);
  if (note) {
    fputc('"', out);
    for (const char * p = note; *p; p++) {
      if (*p == '"' || *p == '\\') {
        fputc('\\', out);
      }
      fputc(*p, out);
    }
    fputc('"', out);
  } else {
    fprintf(out, "null");
  }
  fprintf(out, "}\n");
  fflush(out);
}

int sb_write_pam(const char * path, int width, int height, const uint8_t * pixels, size_t stride, int bgra) {
  FILE * f = fopen(path, "wb");
  if (!f) {
    return -1;
  }
  fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", width, height);
  uint8_t * row = malloc((size_t)width * 4);
  if (!row) {
    fclose(f);
    return -1;
  }
  for (int y = 0; y < height; y++) {
    const uint8_t * src = pixels + (size_t)y * stride;
    for (int x = 0; x < width; x++) {
      row[4 * x + 0] = src[4 * x + (bgra ? 2 : 0)];
      row[4 * x + 1] = src[4 * x + 1];
      row[4 * x + 2] = src[4 * x + (bgra ? 0 : 2)];
      row[4 * x + 3] = src[4 * x + 3];
    }
    fwrite(row, 4, (size_t)width, f);
  }
  free(row);
  return fclose(f) == 0 ? 0 : -1;
}

void sb_background_bytes(const SB_Scene * scene, int bgra, uint8_t out[4]) {
  const SB_Color * c = &scene->background;
  unsigned r = (c->r * c->a + 127u) / 255u, g = (c->g * c->a + 127u) / 255u, b = (c->b * c->a + 127u) / 255u;
  out[0] = (uint8_t)(bgra ? b : r);
  out[1] = (uint8_t)g;
  out[2] = (uint8_t)(bgra ? r : b);
  out[3] = c->a;
}

void sb_clear(uint8_t * pixels, int width, int height, size_t stride, const uint8_t px[4]) {
  for (int y = 0; y < height; y++) {
    uint8_t * row = pixels + (size_t)y * stride;
    for (int x = 0; x < width; x++) {
      memcpy(row + 4 * x, px, 4);
    }
  }
}
