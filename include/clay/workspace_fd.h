#ifndef CLAY_WORKSPACE_FD_H
#define CLAY_WORKSPACE_FD_H

#include <stddef.h>

/* Linux only: open the current working directory as a workspace anchor. */
int clay_workspace_fd_open(void);
/* Paths are nonempty relative paths without dot, dot-dot or empty components.
   On success, read returns an owned file descriptor; all other calls return 0.
   Non-Linux implementations fail with ENOSYS. */
int clay_workspace_fd_read(int root_fd, const char *path);
int clay_workspace_fd_write(int root_fd, const char *path, const void *data, size_t len);
int clay_workspace_fd_unlink(int root_fd, const char *path);

#endif
