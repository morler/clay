#define _GNU_SOURCE
#include "clay/workspace_fd.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <endian.h>
#include <fcntl.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fail_file_sync;
static int directory_syncs;
static int fail_chown;

int fsync(int fd) {
    struct stat st;
    assert(fstat(fd, &st) == 0);
    if (S_ISDIR(st.st_mode)) { ++directory_syncs; errno = EIO; return -1; }
    if (fail_file_sync) { errno = EIO; return -1; }
    return (int)syscall(SYS_fsync, fd);
}

int fchown(int fd, uid_t uid, gid_t gid) {
    if (fail_chown) { errno = EPERM; return -1; }
    return (int)syscall(SYS_fchown, fd, uid, gid);
}

int main(void) {
    char dir[] = "/tmp/clay-workspace-fd-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    int root = clay_workspace_fd_open();
    assert(root >= 0);
    assert(clay_workspace_fd_write(root, "a/b/file", "hello", 5) == 0);
    struct stat st;
    assert(stat("a/b/file", &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(chmod("a/b/file", 0755) == 0);
    assert(clay_workspace_fd_write(root, "a/b/file", "world", 5) == 0);
    assert(stat("a/b/file", &st) == 0 && (st.st_mode & 0777) == 0755);
    assert(directory_syncs == 0);
    fail_file_sync = 1;
    assert(clay_workspace_fd_write(root, "a/b/file", "bad", 3) < 0);
    fail_file_sync = 0;

    gid_t groups[128];
    int ngroups = getgroups(128, groups);
    for (int i = 0; i < ngroups; ++i) {
        if (groups[i] == getegid() || chown("a/b/file", (uid_t)-1, groups[i]) < 0) continue;
        struct stat before, after;
        assert(stat("a/b/file", &before) == 0);
        fail_chown = 1;
        assert(clay_workspace_fd_write(root, "a/b/file", "bad", 3) < 0 && errno == EPERM);
        fail_chown = 0;
        assert(stat("a/b/file", &after) == 0 && after.st_ino == before.st_ino);
        assert(clay_workspace_fd_write(root, "a/b/file", "world", 5) == 0);
        assert(stat("a/b/file", &after) == 0 && after.st_uid == before.st_uid && after.st_gid == before.st_gid);
        break;
    }

    int fd = clay_workspace_fd_read(root, "a/b/file");
    char text[6] = {0};
    assert(fd >= 0 && read(fd, text, 5) == 5 && !strcmp(text, "world"));
    close(fd);
    assert(symlink("a/b", "link") == 0);
    assert(symlink("file", "a/b/link") == 0);
    assert(clay_workspace_fd_write(root, "link/file", "x", 1) < 0);
    assert(clay_workspace_fd_write(root, "a/b/link", "x", 1) < 0);
    assert(clay_workspace_fd_read(root, "a/b/link") < 0);
    assert(clay_workspace_fd_unlink(root, "a/b/link") < 0);
    assert(clay_workspace_fd_write(root, "new/../file", "x", 1) < 0);
    assert(access("new", F_OK) < 0);
    struct {
        struct posix_acl_xattr_header header;
        struct posix_acl_xattr_entry entries[5];
    } acl = {0};
    acl.header.a_version = htole32(POSIX_ACL_XATTR_VERSION);
    const int tags[] = {ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_MASK, ACL_OTHER};
    const int perms[] = {6, 4, 0, 4, 0};
    for (int i = 0; i < 5; ++i) {
        acl.entries[i].e_tag = htole16(tags[i]);
        acl.entries[i].e_perm = htole16(perms[i]);
        acl.entries[i].e_id = htole32(i == 1 ? 12345 : ACL_UNDEFINED_ID);
    }
    if (setxattr("a/b/file", "system.posix_acl_access", &acl, sizeof(acl), 0) == 0) {
        assert(clay_workspace_fd_write(root, "a/b/file", "bad", 3) < 0);
        assert(removexattr("a/b/file", "system.posix_acl_access") == 0);
        assert(setxattr("a/b", "system.posix_acl_default", &acl, sizeof(acl), 0) == 0);
        assert(clay_workspace_fd_write(root, "a/b/file", "bad", 3) < 0);
        assert(removexattr("a/b", "system.posix_acl_default") == 0);
        fd = clay_workspace_fd_read(root, "a/b/file");
        memset(text, 0, sizeof(text));
        assert(fd >= 0 && read(fd, text, 5) == 5 && !strcmp(text, "world"));
        close(fd);
    } else {
        assert(errno == ENOTSUP || errno == EOPNOTSUPP);
        puts("ACL unsupported; ACL tests skipped");
    }
    assert(chmod(dir, 0755) == 0);
    assert(mkdir("search", 0755) == 0 && mkdir("search/inner", 0777) == 0);
    assert(chmod("search/inner", 0777) == 0);
    fd = open("search/inner/file", O_CREAT | O_WRONLY, 0644);
    assert(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
    assert(chmod("search", 0111) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        if (geteuid() == 0) assert(setgid(65534) == 0 && setuid(65534) == 0);
        assert(chdir("search") == 0);
        int child_root = clay_workspace_fd_open();
        assert(child_root >= 0);
        int file = clay_workspace_fd_read(child_root, "inner/file");
        assert(file >= 0 && close(file) == 0);
        assert(clay_workspace_fd_write(child_root, "inner/new", "x", 1) == 0);
        assert(clay_workspace_fd_unlink(child_root, "inner/new") == 0);
        close(child_root);
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(chmod("search", 0700) == 0 && unlink("search/inner/file") == 0);
    assert(rmdir("search/inner") == 0 && rmdir("search") == 0);
    assert(clay_workspace_fd_unlink(root, "a/b/file") == 0);
    assert(unlink("a/b/link") == 0 && unlink("link") == 0);
    assert(rmdir("a/b") == 0 && rmdir("a") == 0);
    close(root);
    assert(chdir("/tmp") == 0 && rmdir(dir) == 0);
    puts("workspace fd tests passed");
    return 0;
}
#else
int main(void) { errno = 0; assert(clay_workspace_fd_open() < 0 && errno == ENOSYS); return 0; }
#endif
