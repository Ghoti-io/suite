/* Does listing DIR fail with EOVERFLOW without large-file support?  Built
 * WITHOUT -D_FILE_OFFSET_BITS=64 by lfs.sh, to learn whether this filesystem
 * can show the failure at all.  Exit 0 if readdir fails with EOVERFLOW. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
int main(int argc, char ** argv) {
  DIR * d = argc > 1 ? opendir(argv[1]) : NULL;
  if (!d) { return 2; }
  struct dirent * e;
  errno = 0;
  while ((e = readdir(d)) != NULL) { }
  int err = errno;
  closedir(d);
  return err == EOVERFLOW ? 0 : 1;
}
