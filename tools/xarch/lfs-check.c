/*
 * Large-file checks for cutil's file, directory and mmap layers, run on a
 * 32-bit target under qemu-user by lfs.sh (notes/cutil/large-files.md).
 *
 *   lfs-check DIR SCRATCH
 *
 * DIR is a directory to list.  SCRATCH is a directory to make sparse files in.
 * Prints "ok   NAME" or "FAIL NAME" per check and exits with the number of
 * failures.  Plain C, because the xarch image has no C++ cross compiler.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/cutil/dir.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/mmap.h>

static int failures;

static void check(int ok, const char * name) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  if (!ok) {
    failures++;
  }
}

/* A sparse file whose last byte is at @p where, so its size is where + 1. */
static int make_sparse(const char * path, int64_t where) {
  GCU_File_Handle h;
  if (gcu_file_open(&h, path, GCU_FILE_OPEN_WRITE, GCU_FILE_PERMS_PRIVATE,
        NULL) != GCU_FILE_OK) {
    return 0;
  }
  int ok = gcu_file_seek(&h, where, GCU_FILE_SEEK_SET) == GCU_FILE_OK;
  ok = ok && gcu_file_write_bytes(&h, "Z", 1) == GCU_FILE_OK;
  int64_t at = -1;
  ok = ok && gcu_file_tell(&h, &at) == GCU_FILE_OK && at == where + 1;
  return gcu_file_close(&h) == GCU_FILE_OK && ok;
}

int main(int argc, char ** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: lfs-check DIR SCRATCH\n");
    return 99;
  }
  printf("sizeof(size_t)=%zu\n", sizeof(size_t));

  /* readdir() fails with EOVERFLOW on a 32-bit build without large-file
   * support, on a directory whose entries carry 64-bit cookies. */
  {
    GCU_Dir dir;
    int entries = 0, bad = 0;
    if (gcu_dir_open(&dir, argv[1], NULL) != GCU_FILE_OK) {
      bad = 1;
    } else {
      for (;;) {
        const char * name;
        bool done = false;
        if (gcu_dir_read(&dir, &name, NULL, &done) != GCU_FILE_OK) {
          bad = 1;
          break;
        }
        if (done) {
          break;
        }
        entries++;
      }
      gcu_dir_close(&dir);
    }
    check(!bad && entries > 0, "list a directory");
  }

  char path[4096];
  const int64_t three_gib = (int64_t)3 << 30;
  snprintf(path, sizeof(path), "%s/lfs-3g", argv[2]);
  int made = make_sparse(path, three_gib);
  check(made, "seek, write and tell at 3 GiB");

  GCU_File_Info info;
  check(made && gcu_file_stat(path, &info) == GCU_FILE_OK &&
          info.size == (uint64_t)three_gib + 1,
    "stat reports the size past 2 GiB");

  {
    GCU_File_Handle h;
    char c = 0;
    size_t got = 0;
    int ok = made &&
      gcu_file_open(&h, path, GCU_FILE_OPEN_READ, GCU_FILE_PERMS_DEFAULT,
        NULL) == GCU_FILE_OK;
    if (ok) {
      ok = gcu_file_seek(&h, -1, GCU_FILE_SEEK_END) == GCU_FILE_OK &&
        gcu_file_read_bytes(&h, &c, 1, &got) == GCU_FILE_OK && got == 1 &&
        c == 'Z';
      gcu_file_close(&h);
    }
    check(ok, "seek back from the end and read the last byte");
  }
  gcu_file_remove(path);

  /* A mapping that does not fit in size_t must be refused, not cut short. */
  const int64_t five_gib = (int64_t)5 << 30;
  snprintf(path, sizeof(path), "%s/lfs-5g", argv[2]);
  if (make_sparse(path, five_gib)) {
    GCU_Mapped_File map;
    int rc = gcu_mmap_open(&map, path, false);
    if (sizeof(size_t) == 4) {
      check(rc != 0, "map a 5 GiB file: refused where size_t is 32 bits");
    } else {
      check(rc == 0 && map.size == (size_t)five_gib + 1,
        "map a 5 GiB file: mapped whole where size_t is 64 bits");
    }
    if (rc == 0) {
      gcu_mmap_close(&map);
    }
  } else {
    check(0, "make a 5 GiB sparse file");
  }
  gcu_file_remove(path);
  return failures;
}
