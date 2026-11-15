#define _GNU_SOURCE
#include "clay/workspace_fd.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <dirent.h>
#include <endian.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/fs.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#if defined(__has_include)
#if __has_include(<linux/openat2.h>) && !defined(CLAY_NO_OPENAT2)
#include <linux/openat2.h>
#define TEST_HAVE_OPENAT2 1
#endif
#endif
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

static int exchange_before_resolve;
static int exchanges;

void clay_workspace_fd_test_before_resolve(int stage) {
#if defined(SYS_renameat2)
    if (exchange_before_resolve == stage) {
        exchange_before_resolve = 0;
        assert(syscall(SYS_renameat2, AT_FDCWD, "a", AT_FDCWD, "race-link", RENAME_EXCHANGE) == 0);
        ++exchanges;
    }
#else
    (void)stage;
#endif
}

static int fail_file_sync;
static int directory_syncs;
static int fail_chown;
static int fail_rename;

static int rename2_error, unlink_error, inject_create;
static int rename2_calls, unlink_interrupts;

int renameat2(int olddir, const char *old, int newdir, const char *new, unsigned int flags) {
    ++rename2_calls;
    if (rename2_error) { errno = rename2_error; return -1; }
    if (inject_create) {
        inject_create = 0;
        int fd = openat(newdir, new, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && write(fd, "external", 8) == 8 && close(fd) == 0);
    }
#ifdef SYS_renameat2
    return (int)syscall(SYS_renameat2, olddir, old, newdir, new, flags);
#else
    errno = ENOSYS; return -1;
#endif
}


int unlinkat(int dir, const char *name, int flags) {
    if (unlink_interrupts && !strncmp(name, ".clay-", 6)) { --unlink_interrupts; errno = EINTR; return -1; }
    if (unlink_error && !strncmp(name, ".clay-", 6)) { errno = unlink_error; return -1; }
    return (int)syscall(SYS_unlinkat, dir, name, flags);
}

int renameat(int olddir, const char *old, int newdir, const char *new) {
    if (fail_rename) { errno = EIO; return -1; }
    return (int)syscall(SYS_renameat, olddir, old, newdir, new);
}

static void assert_no_temp(void) {
    DIR *dir = opendir("a/b");
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) assert(strncmp(entry->d_name, ".clay-", 6));
    assert(closedir(dir) == 0);
}
static const char *fsync_race_target;

int fsync(int fd) {
    struct stat st;
    assert(fstat(fd, &st) == 0);
    if (S_ISDIR(st.st_mode)) { ++directory_syncs; errno = EIO; return -1; }
    if (fail_file_sync) { errno = EIO; return -1; }
    if (fsync_race_target) {
        int target = open(fsync_race_target, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(target >= 0);
        assert(write(target, "concurrent", 10) == 10 && close(target) == 0);
        fsync_race_target = NULL;
    }
    return (int)syscall(SYS_fsync, fd);
}

int fchown(int fd, uid_t uid, gid_t gid) {
    if (fail_chown) { errno = EPERM; return -1; }
    return (int)syscall(SYS_fchown, fd, uid, gid);
}

#if defined(TEST_HAVE_OPENAT2) && defined(SYS_openat2)
static void test_openat2_errors(int root) {
    const struct { int error; int fallback; } cases[] = {
        { ENOSYS, 1 }, { EINVAL, 1 }, { E2BIG, 1 },
        { EPERM, 0 }, { EACCES, 0 }, { ELOOP, 0 }, { EXDEV, 0 },
        { EAGAIN, 0 }, { ENOENT, 0 }, { EIO, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct sock_filter filter[] = {
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat2, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | cases[i].error),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            struct sock_fprog prog = { .len = sizeof(filter) / sizeof(filter[0]), .filter = filter };
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
                prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) _exit(77);
            int fd = clay_workspace_fd_read(root, "a/b/file");
            if (cases[i].fallback) {
                if (fd < 0) _exit(1);
                close(fd);
                if (clay_workspace_fd_read(root, "a/b/link") >= 0 ||
                    clay_workspace_fd_read(root, "link/file") >= 0) _exit(2);
            } else if (fd >= 0 || errno != cases[i].error) _exit(3);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
        if (WEXITSTATUS(status) == 77) {
            puts("seccomp unavailable; injected openat2 error tests skipped");
            return;
        }
        assert(WEXITSTATUS(status) == 0);
    }
}
#endif
int main(void) {
#ifdef CLAY_NO_OPENAT2
    puts("openat2-specific tests skipped: CLAY_NO_OPENAT2 build");
#endif
    char dir[] = "/tmp/clay-workspace-fd-XXXXXX";
    assert(mkdtemp(dir) && chdir(dir) == 0);
    int root = clay_workspace_fd_open();
    assert(root >= 0);
    assert(clay_workspace_fd_write(root, "a/b/file", "hello", 5) == 0);
    struct stat st;
    assert(stat("a/b/file", &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(rename2_calls == 1 && st.st_nlink == 1);
    const int unsupported[] = { ENOSYS, EINVAL, EOPNOTSUPP };
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        rename2_error = unsupported[i];
        assert(clay_workspace_fd_write(root, "a/b/new", "x", 1) < 0 && errno == unsupported[i]);
        assert(access("a/b/new", F_OK) < 0 && errno == ENOENT);
        assert_no_temp();
    }
    unlink_interrupts = 3;
    assert(clay_workspace_fd_write(root, "a/b/new", "x", 1) < 0 && errno == EOPNOTSUPP);
    assert(unlink_interrupts == 0); assert_no_temp();
    unlink_error = EIO;
    assert(clay_workspace_fd_write(root, "a/b/new", "x", 1) < 0 && errno == EOPNOTSUPP);
    assert(access("a/b/new", F_OK) < 0 && errno == ENOENT);
    DIR *temp_dir = opendir("a/b"); assert(temp_dir);
    struct dirent *entry; int found = 0;
    while ((entry = readdir(temp_dir))) if (!strncmp(entry->d_name, ".clay-", 6)) {
        struct stat temp_st;
        assert(fstatat(dirfd(temp_dir), entry->d_name, &temp_st, 0) == 0 && temp_st.st_nlink == 1);
        unlink_error = 0;
        assert(unlinkat(dirfd(temp_dir), entry->d_name, 0) == 0);
        ++found;
    }
    assert(found == 1 && closedir(temp_dir) == 0);
    rename2_error = EACCES;
    assert(clay_workspace_fd_write(root, "a/b/new", "x", 1) < 0 && errno == EACCES);
    assert_no_temp();
    rename2_error = 0;
    inject_create = 1;
    assert(clay_workspace_fd_write(root, "a/b/new", "x", 1) < 0 && errno == EEXIST);
    char buf[9] = {0};
    int race_fd = open("a/b/new", O_RDONLY);
    assert(race_fd >= 0 && read(race_fd, buf, 8) == 8 && !strcmp(buf, "external") && close(race_fd) == 0);
    assert(unlink("a/b/new") == 0); assert_no_temp();
    rename2_error = ENOSYS;
    assert(clay_workspace_fd_write(root, "a/b/file", "world", 5) == 0);
    rename2_error = 0;
    assert(chmod("a/b/file", 0755) == 0);
    assert(clay_workspace_fd_write(root, "a/b/file", "world", 5) == 0);
    assert(stat("a/b/file", &st) == 0 && (st.st_mode & 0777) == 0755);
    assert(directory_syncs == 0);
    fail_file_sync = 1;
    assert(clay_workspace_fd_write(root, "a/b/file", "bad", 3) < 0);
    fail_file_sync = 0;
    assert_no_temp();
    int unchanged = open("a/b/file", O_RDONLY);
    char unchanged_text[6] = {0};
    assert(unchanged >= 0 && read(unchanged, unchanged_text, 5) == 5);
    assert(!strcmp(unchanged_text, "world") && close(unchanged) == 0);
    fail_rename = 1;
    assert(clay_workspace_fd_write(root, "a/b/file", "bad", 3) < 0 && errno == EIO);
    fail_rename = 0;
    assert_no_temp();
    unchanged = open("a/b/file", O_RDONLY);
    memset(unchanged_text, 0, sizeof(unchanged_text));
    assert(unchanged >= 0 && read(unchanged, unchanged_text, 5) == 5);
    assert(!strcmp(unchanged_text, "world") && close(unchanged) == 0);

    {
    fsync_race_target = "race/file";
    errno = 0;
    assert(clay_workspace_fd_write(root, "race/file", "loser", 5) < 0 && errno == EEXIST);
    int race_fd = open("race/file", O_RDONLY);
    char race_text[11] = {0};
    assert(race_fd >= 0 && read(race_fd, race_text, 10) == 10 && !strcmp(race_text, "concurrent"));
    assert(close(race_fd) == 0);
    DIR *race_dir = opendir("race");
    assert(race_dir);
    struct dirent *entry;
    while ((entry = readdir(race_dir)))
        assert(strncmp(entry->d_name, ".clay-", 6) != 0);
    assert(closedir(race_dir) == 0);
    assert(unlink("race/file") == 0 && rmdir("race") == 0);
    }

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

#if defined(TEST_HAVE_OPENAT2) && defined(SYS_openat2)
    struct open_how how = { .flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS };
    int probe = (int)syscall(SYS_openat2, root, "a/b/file", &how, sizeof(how));
    if (probe >= 0) {
        close(probe);
        pid_t filtered = fork();
        assert(filtered >= 0);
        if (filtered == 0) {
            struct sock_filter filter[] = {
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            struct sock_fprog prog = { .len = sizeof(filter) / sizeof(filter[0]), .filter = filter };
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
                prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) _exit(77);
            int file = clay_workspace_fd_read(root, "a/b/file");
            if (file < 0) _exit(1);
            close(file);
            _exit(0);
        }
        int status;
        assert(waitpid(filtered, &status, 0) == filtered && WIFEXITED(status));
        if (WEXITSTATUS(status) == 77) puts("seccomp unavailable; openat2 test skipped");
        else assert(WEXITSTATUS(status) == 0);
    } else puts("openat2 unavailable; openat2 test skipped");
#endif
    const char *invalid[] = {"", "/a/b/file", "a//b/file", "a/./b/file", "a/b/../file", "a/b/file/"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        errno = 0;
        assert(clay_workspace_fd_read(root, invalid[i]) < 0 && errno == EINVAL);
    }
    assert(clay_workspace_fd_read(root, NULL) < 0 && errno == EINVAL);
    assert(chdir("/tmp") == 0);
    int anchored = clay_workspace_fd_read(root, "a/b/file");
    assert(anchored >= 0 && close(anchored) == 0);
    assert(fchdir(root) == 0);
    assert(mkfifo("a/b/fifo", 0600) == 0);
    alarm(3);
    assert(clay_workspace_fd_read(root, "a/b/fifo") < 0 && errno == EINVAL);
    alarm(0);
    assert(unlink("a/b/fifo") == 0);
    assert(clay_workspace_fd_read(root, "a/b") < 0 && errno == EINVAL);
    int fd = clay_workspace_fd_read(root, "a/b/file");
    char text[6] = {0};
    assert(fd >= 0 && read(fd, text, 5) == 5 && !strcmp(text, "world"));
    close(fd);
    assert(symlink("a/b", "link") == 0);
    assert(symlink("file", "a/b/link") == 0);
    assert(clay_workspace_fd_write(root, "link/file", "x", 1) < 0);
    assert(clay_workspace_fd_write(root, "a/b/link", "x", 1) < 0);
    assert(clay_workspace_fd_read(root, "a/b/link") < 0);
    assert(clay_workspace_fd_read(root, "link/file") < 0);
#if defined(TEST_HAVE_OPENAT2) && defined(SYS_openat2)
    test_openat2_errors(root);
#endif
    assert(clay_workspace_fd_unlink(root, "a/b/link") < 0);
#if defined(SYS_renameat2)
    char outside[] = "/tmp/clay-fd-outside-XXXXXX";
    assert(mkdtemp(outside));
    char secret[256], outside_subdir[256];
    snprintf(outside_subdir, sizeof(outside_subdir), "%s/b", outside);
    assert(mkdir(outside_subdir, 0700) == 0);
    snprintf(secret, sizeof(secret), "%s/b/file", outside);
    int secret_fd = open(secret, O_CREAT | O_WRONLY | O_EXCL, 0600);
    assert(secret_fd >= 0 && write(secret_fd, "secret", 6) == 6 && close(secret_fd) == 0);
    assert(symlink(outside, "race-link") == 0);
    if (syscall(SYS_renameat2, AT_FDCWD, "a", AT_FDCWD, "race-link", RENAME_EXCHANGE) == 0) {
        assert(syscall(SYS_renameat2, AT_FDCWD, "a", AT_FDCWD, "race-link", RENAME_EXCHANGE) == 0);
        for (int operation = 0; operation < 3; ++operation) {
            exchange_before_resolve = 1;
            int rc = operation == 0 ? clay_workspace_fd_read(root, "a/b/file") :
                     operation == 1 ? clay_workspace_fd_write(root, "a/b/file", "bad", 3) :
                                      clay_workspace_fd_unlink(root, "a/b/file");
            assert(rc < 0 && exchanges == operation + 1 && !exchange_before_resolve);
            char buf[8] = {0};
            int outside_fd = open(secret, O_RDONLY);
            assert(outside_fd >= 0 && read(outside_fd, buf, sizeof(buf)) == 6 && !strcmp(buf, "secret"));
            assert(close(outside_fd) == 0);
            assert(syscall(SYS_renameat2, AT_FDCWD, "a", AT_FDCWD, "race-link", RENAME_EXCHANGE) == 0);
        }
        puts("deterministic pre-resolution read/write/unlink exchange passed");
        for (int operation = 0; operation < 3; ++operation) {
#ifndef CLAY_NO_OPENAT2
            if (operation == 0) continue;
#endif
            exchange_before_resolve = 2;
            int previous = exchanges;
            int rc = operation == 0 ? clay_workspace_fd_read(root, "a/b/file") :
                     operation == 1 ? clay_workspace_fd_write(root, "a/b/file", "inside", 6) :
                                      clay_workspace_fd_unlink(root, "a/b/file");
            assert(rc >= 0 && exchanges == previous + 1 && !exchange_before_resolve);
            if (operation == 0) {
                char buf[8] = {0};
                assert(read(rc, buf, sizeof(buf)) == 5 && !strcmp(buf, "world"));
                assert(close(rc) == 0);
            }
            char buf[8] = {0};
            int outside_fd = open(secret, O_RDONLY);
            assert(outside_fd >= 0 && read(outside_fd, buf, sizeof(buf)) == 6 && !strcmp(buf, "secret"));
            assert(close(outside_fd) == 0);
            assert(syscall(SYS_renameat2, AT_FDCWD, "a", AT_FDCWD, "race-link", RENAME_EXCHANGE) == 0);
            if (operation != 0) assert(clay_workspace_fd_write(root, "a/b/file", "world", 5) == 0);
        }
        puts("deterministic pinned-parent exchange passed");
        pid_t racer = fork();
        assert(racer >= 0);
        if (racer == 0) {
            for (int i = 0; i < 10000; i++)
                if (syscall(SYS_renameat2, AT_FDCWD, "a", AT_FDCWD, "race-link", RENAME_EXCHANGE) < 0)
                    _exit(1);
            _exit(0);
        }
        for (int i = 0; i < 3000; i++) {
            int file = clay_workspace_fd_read(root, "a/b/file");
            if (file >= 0) {
                char buf[8] = {0};
                assert(read(file, buf, sizeof(buf)) == 5 && !strcmp(buf, "world"));
                close(file);
            }
        }
        int status;
        assert(waitpid(racer, &status, 0) == racer && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    } else {
        assert(errno == ENOSYS || errno == EINVAL || errno == EPERM || errno == EOPNOTSUPP);
        puts("renameat2 exchange unavailable; race test skipped");
    }
    assert(unlink("race-link") == 0 && unlink(secret) == 0 && rmdir(outside_subdir) == 0 && rmdir(outside) == 0);
#else
    puts("renameat2 unavailable at build time; deterministic exchange and race test skipped");
#endif
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
