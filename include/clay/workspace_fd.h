#ifndef CLAY_WORKSPACE_FD_H
#define CLAY_WORKSPACE_FD_H

#include <stddef.h>

/* Linux only: open the current working directory as a workspace anchor. */
int clay_workspace_fd_open(void);
/* Paths are nonempty relative paths without dot, dot-dot or empty components.
   On success, read returns an owned file descriptor; all other calls return 0.
   Linux read falls back to component-wise openat on ENOSYS, EINVAL or E2BIG;
   all other openat2 errors, including EPERM, EACCES, ELOOP, EXDEV and EAGAIN,
   fail closed. A seccomp filter returning EPERM therefore prevents reads.
   The fallback does not detect an opened directory being moved outside root;
   writes and unlinks also use component-wise traversal and have this limitation.
   Non-Linux implementations fail with ENOSYS. */
int clay_workspace_fd_read(int root_fd, const char *path);
int clay_workspace_fd_write(int root_fd, const char *path, const void *data, size_t len);
int clay_workspace_fd_unlink(int root_fd, const char *path);

#endif
