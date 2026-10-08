/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * Windows probe for security: exercises the `_WIN32` arm of
 * gsec_random_bytes, which no Linux build compiles.
 *
 * What a green run proves: the arm compiles, BCryptGenRandom is called with
 * argument types and flags it accepts, its NTSTATUS is read correctly, the
 * buffer really is filled, and the three refusal branches that return before
 * the call still refuse.  gsec_selftest() then runs every primitive.
 *
 * What it does not prove: anything about the generator on a real Windows
 * machine.  Under wine, bcrypt.dll is wine's own implementation.  This
 * answers "does our code work there", not "does Windows behave as assumed".
 */
#include <ghoti.io/security/security.h>
#include <ghoti.io/security/random.h>
#include <ghoti.io/security/core.h>
#include <ghoti.io/security/selftest.h>
#include <ghoti.io/cutil/random.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
static void ck(const char * what, int cond) {
  printf("%-46s %s\n", what, cond ? "ok" : "FAIL");
  if (!cond) { fails++; }
}

int main(void) {
  unsigned char a[32], b[32], big[8192];
  unsigned char canary[8];
  GSEC_Limits lim;
  GCU_Random * r;
  int seen[256];
  int distinct = 0, i, zrun = 0, maxzrun = 0;

  printf("gsec version: %s\n", gsec_version_string());

  memset(a, 0, sizeof a); memset(b, 0, sizeof b);
  ck("32 bytes returns GSEC_OK", gsec_random_bytes(a, sizeof a, NULL) == GSEC_OK);
  ck("32 bytes is not all zero", memcmp(a, "\0\0\0\0\0\0\0\0", 8) != 0 || a[8] || a[9]);
  ck("second call returns GSEC_OK", gsec_random_bytes(b, sizeof b, NULL) == GSEC_OK);
  ck("two draws differ", memcmp(a, b, sizeof a) != 0);

  ck("n == 0 returns GSEC_OK", gsec_random_bytes(a, 0, NULL) == GSEC_OK);
  ck("NULL out returns GSEC_ERR_INVALID",
      gsec_random_bytes(NULL, 1, NULL) == GSEC_ERR_INVALID);

  gsec_limits_default(&lim);
  ck("over max_random_bytes returns GSEC_ERR_LIMIT",
      gsec_random_bytes(a, lim.max_random_bytes + 1u, NULL) == GSEC_ERR_LIMIT);

  /* The ULONG guard: reachable only when the caller raises the cap above
   * 2^32.  Returns before writing, so a small buffer is safe here. */
  memset(canary, 0xA5, sizeof canary);
  lim.max_random_bytes = (size_t)-1;
  if (sizeof(size_t) > 4) {
    ck("n > 0xFFFFFFFF returns GSEC_ERR_LIMIT",
        gsec_random_bytes(canary, (size_t)0x100000000ULL, &lim) == GSEC_ERR_LIMIT);
    ck("that guard wrote nothing", canary[0] == 0xA5 && canary[7] == 0xA5);
  }

  /* Crude quality check: 8 KB should show nearly every byte value and no
   * long zero run.  This is a "did it actually fill the buffer" test, not a
   * statistical test of the kernel's generator. */
  memset(big, 0, sizeof big);
  ck("8192 bytes returns GSEC_OK", gsec_random_bytes(big, sizeof big, NULL) == GSEC_OK);
  memset(seen, 0, sizeof seen);
  for (i = 0; i < (int)sizeof big; i++) {
    if (!seen[big[i]]) { seen[big[i]] = 1; distinct++; }
    if (big[i] == 0) { zrun++; if (zrun > maxzrun) { maxzrun = zrun; } } else { zrun = 0; }
  }
  printf("  distinct byte values in 8192: %d, longest zero run: %d\n", distinct, maxzrun);
  ck("8192 bytes shows >= 250 distinct values", distinct >= 250);
  ck("no 16-byte zero run", maxzrun < 16);

  r = gsec_random_open();
  ck("gsec_random_open returns a handle", r != NULL);
  if (r != NULL) { gcu_random_free(r); }

  ck("gsec_selftest passes", gsec_selftest() == GSEC_OK);

  printf("\n%s (%d failures)\n", fails ? "FAILED" : "all clear", fails);
  return fails ? 1 : 0;
}
