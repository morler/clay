#define _GNU_SOURCE
#include "clay/workspace_fd.h"

#include <errno.h>
#ifdef __linux__
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int parent_fd(int root, const char *path, int create, char **name) {
    if (!path || !*path || *path == '/') { errno = EINVAL; return -1; }
    const char *component = path;
    for (const char *p = path;; ++p) {
        if (*p != '/' && *p) continue;
        size_t n = (size_t)(p - component);
        if (!n || (n == 1 && component[0] == '.') ||
            (n == 2 && component[0] == '.' && component[1] == '.')) {
            errno = EINVAL; return -1;
        }
        if (!*p) break;
        component = p + 1;
    }
    char *copy = strdup(path);
    if (!copy) return -1;
    int dir = fcntl(root, F_DUPFD_CLOEXEC, 0);
    if (dir < 0) { free(copy); return -1; }
    char *part = copy;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { errno = EINVAL; goto fail; }
        if (!slash) {
            *name = strdup(part);
            if (!*name) goto fail;
            free(copy);
            return dir;
        }
        int next = openat(dir, part, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(dir, part, 0777) < 0 && errno != EEXIST) goto fail;
            next = openat(dir, part, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) goto fail;
        close(dir);
        dir = next;
        part = slash + 1;
    }
fail: {
    int error = errno;
    close(dir);
    free(copy);
    errno = error;
    return -1;
}}

int clay_workspace_fd_open(void) {
    return open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
}

int clay_workspace_fd_read(int root, const char *path) {
    char *name;
    int dir = parent_fd(root, path, 0, &name);
    if (dir < 0) return -1;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    int error = errno;
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) < 0) { error = errno; close(fd); fd = -1; }
        else if (!S_ISREG(st.st_mode)) { error = EINVAL; close(fd); fd = -1; }
    }
    close(dir);
    free(name);
    errno = error;
    return fd;
}

int clay_workspace_fd_unlink(int root, const char *path) {
    char *name;
    int dir = parent_fd(root, path, 0, &name);
    if (dir < 0) return -1;
    struct stat st;
    int rc = fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW);
    if (!rc && !S_ISREG(st.st_mode)) { errno = EINVAL; rc = -1; }
    if (!rc) rc = unlinkat(dir, name, 0);
    int error = errno;
    close(dir);
    free(name);
    errno = error;
    return rc;
}

int clay_workspace_fd_write(int root, const char *path, const void *data, size_t len) {
    if (!data && len) { errno = EINVAL; return -1; }
    char *name;
    int dir = parent_fd(root, path, 1, &name);
    if (dir < 0) return -1;
    int old = -1, tmp = -1, rc = -1, created = 0;
    char temp[40];
    struct stat st;
    old = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (old >= 0) {
        if (fstat(old, &st) < 0) goto done;
        if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto done; }
        /* shortcut: reject ACLs rather than lose them; use an ACL API to preserve them. */
        if (fgetxattr(old, "system.posix_acl_access", NULL, 0) >= 0) { errno = ENOTSUP; goto done; }
        if (errno != ENODATA) goto done;
    } else if (errno != ENOENT) goto done;
    for (int i = 0; i < 128; ++i) {
        uint64_t random;
        ssize_t n;
        do { n = getrandom(&random, sizeof(random), 0); } while (n < 0 && errno == EINTR);
        if (n != sizeof(random)) { if (n >= 0) errno = EIO; goto done; }
        snprintf(temp, sizeof(temp), ".clay-%016llx.tmp", (unsigned long long)random);
        tmp = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (tmp >= 0) { created = 1; break; }
        if (errno != EEXIST) goto done;
    }
    if (tmp < 0) { errno = EEXIST; goto done; }
    if (old >= 0) {
        if (fgetxattr(tmp, "system.posix_acl_access", NULL, 0) >= 0) { errno = ENOTSUP; goto done; }
        if (errno != ENODATA) goto done;
        struct stat new_st;
        if (fstat(tmp, &new_st) < 0) goto done;
        if ((new_st.st_uid != st.st_uid || new_st.st_gid != st.st_gid) &&
            fchown(tmp, st.st_uid, st.st_gid) < 0) goto done;
        if (fchmod(tmp, st.st_mode & 0777) < 0) goto done;
    }
    const char *p = data;
    while (len) {
        ssize_t n = write(tmp, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; goto done; }
        p += n; len -= (size_t)n;
    }
    if (fsync(tmp) < 0) goto done;
    if (close(tmp) < 0) { tmp = -1; goto done; }
    tmp = -1;
    if (renameat(dir, temp, dir, name) < 0) goto done;
    created = 0;
    /* The rename commits the change; directory durability is not guaranteed. */
    rc = 0;
done: {
    int error = errno;
    if (tmp >= 0) close(tmp);
    if (created) unlinkat(dir, temp, 0);
    if (old >= 0) close(old);
    close(dir);
    free(name);
    errno = error;
    return rc;
}}
#else
int clay_workspace_fd_open(void) { errno = ENOSYS; return -1; }
int clay_workspace_fd_read(int root, const char *path) { (void)root; (void)path; errno = ENOSYS; return -1; }
int clay_workspace_fd_write(int root, const char *path, const void *data, size_t len) {
    (void)root; (void)path; (void)data; (void)len; errno = ENOSYS; return -1;
}
int clay_workspace_fd_unlink(int root, const char *path) { (void)root; (void)path; errno = ENOSYS; return -1; }
#endif
