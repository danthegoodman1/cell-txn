// LD_PRELOAD shim: the FAIL_SYNC_AT-th sync and every later one fail with
// EIO. A sync is an fdatasync or a pwritev2 with RWF_DSYNC.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/uio.h>

static atomic_long syncs;

static int failing(void) {
  const char *at = getenv("FAIL_SYNC_AT");
  return at && atomic_fetch_add(&syncs, 1) + 1 >= atol(at);
}

int fdatasync(int fd) {
  static int (*real)(int);
  if (!real) real = (int (*)(int))dlsym(RTLD_NEXT, "fdatasync");
  if (failing()) {
    errno = EIO;
    return -1;
  }
  return real(fd);
}

ssize_t pwritev2(int fd, const struct iovec *iov, int n, off_t off, int flags) {
  static ssize_t (*real)(int, const struct iovec *, int, off_t, int);
  if (!real) real = (ssize_t (*)(int, const struct iovec *, int, off_t, int))dlsym(RTLD_NEXT, "pwritev2");
  if ((flags & RWF_DSYNC) && failing()) {
    errno = EIO;
    return -1;
  }
  return real(fd, iov, n, off, flags);
}
