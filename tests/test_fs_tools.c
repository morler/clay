#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/resource.h>

static int walk_swap;
static const char *walk_outside;
struct dirent *__real_readdir(DIR *dir);
struct dirent *__wrap_readdir(DIR *dir) {
    struct dirent *entry = __real_readdir(dir);
    if (walk_swap && entry && !strcmp(entry->d_name, "probe.c")) {
        int mode = walk_swap;
        walk_swap = 0;
        if (mode == 3) assert(chmod("walk-parent", 0) == 0);
        else {
            assert(rename("walk-parent", "walk-saved") == 0);
            if (mode == 1) assert(symlink(walk_outside, "walk-parent") == 0);
            if (mode == 4) {
                assert(mkdir("walk-parent", 0700) == 0);
                int fd = open("walk-parent/probe.c", O_CREAT | O_WRONLY | O_EXCL, 0600);
                assert(fd >= 0 && close(fd) == 0);
            }
        }
    }
    return entry;
}
static int inject_commit_change;
static int inject_create_change;
int renameat2(int olddir, const char *old, int newdir, const char *new, unsigned int flags) {
    if (inject_create_change && !strcmp(new, "create-race")) {
        inject_create_change = 0;
        int fd = openat(newdir, new, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && write(fd, "external", 8) == 8 && close(fd) == 0);
    }
    return (int)syscall(SYS_renameat2, olddir, old, newdir, new, flags);
}
int linkat(int olddir, const char *old, int newdir, const char *new, int flags) {
    if (inject_create_change && !strcmp(new, "create-race")) {
        inject_create_change = 0;
        int fd = openat(newdir, new, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && write(fd, "external", 8) == 8 && close(fd) == 0);
    }
    return (int)syscall(SYS_linkat, olddir, old, newdir, new, flags);
}
int renameat(int olddir, const char *old, int newdir, const char *new) {
    if (inject_commit_change && !strcmp(new, "commit-race")) {
        inject_commit_change = 0;
        int fd = openat(newdir, new, O_WRONLY | O_TRUNC);
        assert(fd >= 0 && write(fd, "external", 8) == 8 && close(fd) == 0);
    }
    return (int)syscall(SYS_renameat, olddir, old, newdir, new);
}
#endif

#ifdef __linux__
static struct stat swap_target;
static int swap_armed, swap_done;
static int grep_swap_armed, grep_swap_done;
static const char *grep_swap_secret;
static int grep_dir_swap_armed, grep_dir_swap_done;
static const char *grep_dir_swap_outside;
int __real_fstat(int fd, struct stat *st);
int __wrap_fstat(int fd, struct stat *st) {
    int rc = __real_fstat(fd, st);
    if (!rc && swap_armed && st->st_dev == swap_target.st_dev && st->st_ino == swap_target.st_ino &&
        (fcntl(fd, F_GETFL) & O_PATH) == O_PATH) {
        swap_armed = 0;
        assert(rename("glob-swap/target", "glob-swap/old") == 0);
        assert(rename("glob-swap-replacement", "glob-swap/target") == 0);
        swap_done = 1;
    }
    return rc;
}

int __real_clay_sandbox_exec_input(const ClaySandboxConfig *, const char *, ClayStr *, size_t, const ClayExecOptions *, ClayExecResult *, int);
int __wrap_clay_sandbox_exec_input(const ClaySandboxConfig *config, const char *command, ClayStr *output, size_t output_limit, const ClayExecOptions *options, ClayExecResult *result, int input_fd) {
    if (grep_swap_armed) {
        grep_swap_armed = 0;
        assert(rename("grep-race.txt", "grep-race-old.txt") == 0);
        assert(symlink(grep_swap_secret, "grep-race.txt") == 0);
        grep_swap_done = 1;
    }
    if (grep_dir_swap_armed) {
        grep_dir_swap_armed = 0;
        assert(rename("grep-dir-race", "grep-dir-saved") == 0);
        assert(symlink(grep_dir_swap_outside, "grep-dir-race") == 0);
        grep_dir_swap_done = 1;
    }
    return __real_clay_sandbox_exec_input(config, command, output, output_limit, options, result, input_fd);
}
#endif

typedef ClayJson *(*ClayFsTool)(const ClayJson *arguments, void *userdata);

static ClayJson *call(ClayFsTool fn, const char *json_args) {
    ClayJson *args = clay_json_parse(json_args, NULL);
    ClayJson *result = fn(args, NULL);
    clay_json_free(args);
    return result;
}

static int ok(ClayJson *result) {
    return clay_json_bool_value(clay_json_object_get(result, "ok"));
}

static void test_windows_paths(void) {
    static const struct { const char *base, *path, *expected; } cases[] = {
        {"C:\\Work\\Repo", "dir\\..\\file", "C:/Work/Repo/file"},
        {"C:/Work/Repo/", "c:\\work\\repo\\file", NULL},
        {"C:/", "x/../a", "C:/a"},
        {"\\\\Server\\Share\\Repo", "sub/file", "//Server/Share/Repo/sub/file"},
        {"//server/share", "//SERVER/SHARE/file", NULL},
        {"C:/work/repo", "../outside", NULL},
        {"C:/work/repo", "C:/work/repository/file", NULL},
        {"C:/work/repo", "D:/work/repo/file", NULL},
        {"C:/work/repo", "C:relative", NULL},
        {"C:/work/repo", "/rooted", NULL},
        {"C:/work/repo", "../REPO/secret", NULL},
        {"C:/work/repo", "NUL.txt", NULL},
        {"C:/work/repo", "dir/CoM1.log", NULL},
        {"C:/work/repo", "LPT².txt", NULL},
        {"C:/work/repo", "con .txt", NULL},
        {"C:/work/repo", "conin$", NULL},
        {"C:/work/repo", "CLOCK$", NULL},
        {"C:/work/repo", "com0.txt", "C:/work/repo/com0.txt"},
        {"C:/work/repo", "file.", NULL},
        {"C:/work/repo", "file ", NULL},
        {"C:/work/repo", "file:stream", NULL},
        {"C:/work/repo", "dir. /file", NULL},
        {"C:/work/repo", "//?/C:/work/repo/file", NULL},
        {"//server/share/repo", "//server/share2/repo/file", NULL},
        {"//server/share/repo", "../../other", NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ClayStr out;
        int rc = clay_fs_resolve_windows_path(cases[i].base, cases[i].path, &out);
        assert((rc == 0) == (cases[i].expected != NULL));
        if (cases[i].expected) assert(strcmp(out.data, cases[i].expected) == 0);
        clay_str_free(&out);
    }
}
#ifdef __linux__
static void test_grep_race(ClayCommands *commands) {
    /* Pin the regular-file fd before a deterministic path swap. */
    char race_secret[] = "/tmp/clay_grep_race_XXXXXX";
    int secret_fd = mkstemp(race_secret);
    assert(secret_fd >= 0);
    FILE *race_fixture = fdopen(secret_fd, "w");
    assert(race_fixture && fputs("grep_race_external_secret\n", race_fixture) >= 0 && fclose(race_fixture) == 0);
    grep_swap_secret = race_secret;
    race_fixture = fopen("grep-race.txt", "w");
    assert(race_fixture && fputs("grep_race_original\n", race_fixture) >= 0 && fclose(race_fixture) == 0);
    grep_swap_done = 0;
    grep_swap_armed = 1;
    ClayJson *race_args = clay_json_parse("{\"pattern\":\"grep_race_\",\"path\":\"grep-race.txt\",\"glob\":\"*.txt\"}", NULL);
    ClayJson *result = clay_fs_tool_grep(race_args, commands);
    assert(ok(result));
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(grep_swap_done && output && strstr(output, "grep_race_original"));
    assert(!strstr(output, "grep_race_external_secret"));
    clay_json_free(result); clay_json_free(race_args);
    assert(unlink("grep-race.txt") == 0 && unlink("grep-race-old.txt") == 0);
    assert(unlink(race_secret) == 0);
}
#endif

static void test_grep_directory_race_baseline(ClayCommands *commands) {
    char *scratch = clay_chat_scratch_dir(commands->chat);
    assert(scratch);
    char outside_template[512];
    snprintf(outside_template, sizeof(outside_template),
             "%s/clay_grep_dir_outside_XXXXXX", scratch);
    char *outside = mkdtemp(outside_template);
    assert(outside);
    free(scratch);
    char secret[512];
    snprintf(secret, sizeof(secret), "%s/secret", outside);
    FILE *file = fopen(secret, "w");
    assert(file && fputs("race-dir-secret\n", file) >= 0 && fclose(file) == 0);
    assert(mkdir("grep-dir-race", 0700) == 0);
    file = fopen("grep-dir-race/inside", "w");
    assert(file && fputs("race-dir-inside\n", file) >= 0 && fclose(file) == 0);

    grep_dir_swap_outside = outside;
    grep_dir_swap_done = 0;
    grep_dir_swap_armed = 1;
    ClayJson *args = clay_json_parse(
        "{\"pattern\":\"race-dir-\",\"path\":\"grep-dir-race\"}", NULL);
    ClayJson *result = clay_fs_tool_grep(args, commands);
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    printf("grep directory race baseline: %s\n",
           output && strstr(output, "race-dir-secret") ? "outside content exposed"
                                                        : "outside content not observed");
    if (getenv("CLAY_ASSERT_GREP_RACE_SAFETY")) {
        assert(grep_dir_swap_done);
        assert(!output || !strstr(output, "race-dir-secret"));
    }
    clay_json_free(result);
    clay_json_free(args);

    struct stat st;
    assert(lstat("grep-dir-race", &st) == 0);
    if (S_ISLNK(st.st_mode)) {
        assert(unlink("grep-dir-race") == 0);
        assert(unlink("grep-dir-saved/inside") == 0);
        assert(rmdir("grep-dir-saved") == 0);
    } else {
        assert(unlink("grep-dir-race/inside") == 0);
        assert(rmdir("grep-dir-race") == 0);
    }
    assert(unlink(secret) == 0 && rmdir(outside) == 0);
    grep_dir_swap_outside = NULL;
}

int main(void) {
    test_windows_paths();
    static const struct { const char *path, *parent; } parents[] = {
        {"C:/file", "C:/"}, {"C:\\file", "C:\\"},
        {"C:/dir/file", "C:/dir"}, {"//server/share/file", "//server/share"},
        {"file", "."}, {"/file", "/"},
    };
    for (size_t i = 0; i < sizeof(parents) / sizeof(parents[0]); i++) {
        ClayStr parent;
        clay_term_windows_parent_dir(parents[i].path, &parent);
        assert(strcmp(parent.data, parents[i].parent) == 0);
        clay_str_free(&parent);
    }
    char template[] = "/tmp/clay_test_fs_XXXXXX";
    char *workspace = mkdtemp(template);
    assert(workspace);
    assert(chdir(workspace) == 0);

    ClayJson *result = call(clay_fs_tool_write, "{\"path\":\"a.txt\",\"content\":\"line1\\nline2\\n\"}");
    assert(ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_read, "{\"path\":\"a.txt\"}");
    assert(ok(result));
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "line1"));
    assert(strstr(output, "line2"));
    clay_json_free(result);

#ifdef __linux__
    char walk_template[] = "/tmp/clay_glob_outside_XXXXXX";
    walk_outside = mkdtemp(walk_template); assert(walk_outside);
    char decoy[512];
    snprintf(decoy, sizeof(decoy), "%s/probe.c", walk_outside);
    FILE *walk_file = fopen(decoy, "w"); assert(walk_file);
    assert(fputs("outside", walk_file) >= 0 && fclose(walk_file) == 0);
    for (int mode = 1; mode <= 4; mode++) {
        if (mode == 3 && geteuid() == 0) {
            fprintf(stderr, "glob permission test skipped: root may bypass permissions\n");
            continue;
        }
        assert(mkdir("walk-parent", 0700) == 0);
        walk_file = fopen("walk-parent/probe.c", "w"); assert(walk_file);
        assert(fputs("inside", walk_file) >= 0 && fclose(walk_file) == 0);
        walk_swap = mode;
        result = call(clay_fs_tool_glob, "{\"pattern\":\"*.c\"}");
        assert(walk_swap == 0 && ok(result));
        assert(!!strstr(clay_json_string_value(clay_json_object_get(result, "output")), "walk-parent/probe.c") == (mode == 4));
        clay_json_free(result);
        if (mode == 3) {
            assert(chmod("walk-parent", 0700) == 0);
            assert(unlink("walk-parent/probe.c") == 0 && rmdir("walk-parent") == 0);
        } else {
            if (mode == 1) assert(unlink("walk-parent") == 0);
            if (mode == 4) assert(unlink("walk-parent/probe.c") == 0 && rmdir("walk-parent") == 0);
            assert(unlink("walk-saved/probe.c") == 0 && rmdir("walk-saved") == 0);
        }
    }
    assert(unlink(decoy) == 0 && rmdir(walk_outside) == 0);
    assert(mkfifo("read-fifo", 0600) == 0);
    alarm(3);
    result = call(clay_fs_tool_read, "{\"path\":\"read-fifo\"}");
    alarm(0);
    assert(!ok(result));
    clay_json_free(result);
    assert(unlink("read-fifo") == 0);
    result = call(clay_fs_tool_read, "{\"path\":\".\"}");
    assert(!ok(result));
    clay_json_free(result);

    assert(symlink("a.txt", "a.tx-link") == 0);
#if defined(SYS_openat2) && defined(__has_include)
#if __has_include(<linux/openat2.h>) && !defined(CLAY_NO_OPENAT2)
    const struct { int error; int fallback; } cases[] = {
        { ENOSYS, 1 }, { EINVAL, 1 }, { E2BIG, 1 },
        { EPERM, 0 }, { EACCES, 0 }, { ELOOP, 0 }, { EXDEV, 0 },
        { EAGAIN, 0 }, { EIO, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pid_t filtered = fork();
        assert(filtered >= 0);
        if (filtered == 0) {
            struct sock_filter filter[] = {
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat2, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | cases[i].error),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            struct sock_fprog prog = { .len = sizeof(filter) / sizeof(filter[0]), .filter = filter };
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) _exit(77);
            ClayJson *r = call(clay_fs_tool_read, "{\"path\":\"a.txt\"}");
            int success = ok(r) == cases[i].fallback;
            if (!cases[i].fallback) {
                const char *error = clay_json_string_value(clay_json_object_get(r, "error"));
                success = success && error && strstr(error, strerror(cases[i].error));
            }
            clay_json_free(r);
            r = call(clay_fs_tool_read, "{\"path\":\"a.tx\"}");
            const char *message = clay_json_string_value(clay_json_object_get(r, "error"));
            success = success && !ok(r) && message && (!!strstr(message, "a.txt") == cases[i].fallback) && !strstr(message, "a.tx-link");
            clay_json_free(r);
            r = call(clay_fs_tool_glob, "{\"pattern\":\"*\"}");
            const char *listing = clay_json_string_value(clay_json_object_get(r, "output"));
            success = success && ok(r) && listing && (!!strstr(listing, "a.txt") == cases[i].fallback) && !strstr(listing, "a.tx-link");
            clay_json_free(r);
            _exit(success ? 0 : 1);
        }
        int filtered_status;
        assert(waitpid(filtered, &filtered_status, 0) == filtered);
        assert(WIFEXITED(filtered_status));
        if (WEXITSTATUS(filtered_status) == 77) printf("seccomp unavailable; skipped\n");
        else assert(WEXITSTATUS(filtered_status) == 0);
    }
#endif
#endif
    result = call(clay_fs_tool_read, "{\"path\":\"a.tx\"}");
    assert(!ok(result));
    assert(!strstr(clay_json_string_value(clay_json_object_get(result, "error")), "a.tx-link"));
    clay_json_free(result);
    assert(unlink("a.tx-link") == 0);

    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    assert(cwd >= 0);
    for (int i = 0; i < 40; i++) {
        assert(mkdir("deep", 0700) == 0);
        assert(chdir("deep") == 0);
    }
    FILE *deep_file = fopen("deep-leaf.txt", "w"); assert(deep_file && fclose(deep_file) == 0);
    assert(fchdir(cwd) == 0);
    struct rlimit old_limit, low_limit;
    assert(getrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    low_limit = old_limit;
    if (low_limit.rlim_cur > 32) low_limit.rlim_cur = 32;
    assert(setrlimit(RLIMIT_NOFILE, &low_limit) == 0);
    result = call(clay_fs_tool_glob, "{\"pattern\":\"*.txt\"}");
    assert(ok(result) && strstr(clay_json_string_value(clay_json_object_get(result, "output")), "deep-leaf.txt"));
    clay_json_free(result);
    result = call(clay_fs_tool_read, "{\"path\":\"a.tx\"}");
    assert(!ok(result));
    assert(strstr(clay_json_string_value(clay_json_object_get(result, "error")), "a.txt"));
    assert(strstr(clay_json_string_value(clay_json_object_get(result, "error")), "suggestion search depth limit reached"));
    clay_json_free(result);
    result = call(clay_fs_tool_read, "{\"path\":\"no-match\"}");
    assert(!ok(result));
    assert(strstr(clay_json_string_value(clay_json_object_get(result, "error")), "suggestion search depth limit reached"));
    clay_json_free(result);
    assert(setrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    for (int i = 0; i < 40; i++) assert(chdir("deep") == 0);
    assert(unlink("deep-leaf.txt") == 0);
    for (int i = 0; i < 40; i++) {
        assert(chdir("..") == 0);
        assert(rmdir("deep") == 0);
    }
    close(cwd);

    /* Swap a directory with an escaping symlink while reads are in flight. */
    char outside[] = "/tmp/clay_read_outside_XXXXXX";
    assert(mkdtemp(outside));
    char secret[512];
    snprintf(secret, sizeof(secret), "%s/file", outside);
    FILE *f = fopen(secret, "w");
    assert(f); assert(fputs("outside-secret\n", f) >= 0); assert(fclose(f) == 0);
    assert(mkdir("race", 0700) == 0);
    f = fopen("race/file", "w");
    assert(f); assert(fputs("inside\n", f) >= 0); assert(fclose(f) == 0);
    char secret_name[512];
    snprintf(secret_name, sizeof(secret_name), "%s/sensitive-unique", outside);
    f = fopen(secret_name, "w");
    assert(f); assert(fclose(f) == 0);
    assert(symlink(outside, "race-link") == 0);
#if defined(SYS_renameat2)
    int exchange = syscall(SYS_renameat2, AT_FDCWD, "race", AT_FDCWD, "race-link", RENAME_EXCHANGE);
    if (exchange == 0) {
        assert(syscall(SYS_renameat2, AT_FDCWD, "race", AT_FDCWD, "race-link", RENAME_EXCHANGE) == 0);
    } else {
        assert(errno == ENOSYS || errno == EINVAL || errno == EPERM || errno == EOPNOTSUPP);
        printf("renameat2 exchange unavailable; race test skipped\n");
    }
    if (exchange == 0) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        for (int i = 0; i < 20000; i++)
            if (syscall(SYS_renameat2, AT_FDCWD, "race", AT_FDCWD, "race-link", RENAME_EXCHANGE) != 0)
                _exit(1);
        _exit(0);
    }
    for (int i = 0; i < 3000; i++) {
        result = call(clay_fs_tool_read, "{\"path\":\"race/file\"}");
        if (ok(result)) {
            output = clay_json_string_value(clay_json_object_get(result, "output"));
            assert(strstr(output, "inside"));
            assert(!strstr(output, "outside-secret"));
        }
        clay_json_free(result);
    }
    for (int i = 0; i < 100; i++) {
        result = call(clay_fs_tool_read, "{\"path\":\"sensitive\"}");
        assert(!ok(result));
        assert(!strstr(clay_json_string_value(clay_json_object_get(result, "error")), "sensitive-unique"));
        clay_json_free(result);
    }
    pid_t glob_child = fork();
    assert(glob_child >= 0);
    if (glob_child == 0) {
        for (int i = 0; i < 40000; i++)
            if (syscall(SYS_renameat2, AT_FDCWD, "race", AT_FDCWD, "race-link", RENAME_EXCHANGE) != 0)
                _exit(1);
        _exit(0);
    }
    for (int i = 0; i < 1000; i++) {
        result = call(clay_fs_tool_glob, "{\"pattern\":\"*\"}");
        assert(ok(result));
        output = clay_json_string_value(clay_json_object_get(result, "output"));
        assert(!strstr(output, "sensitive-unique"));
        clay_json_free(result);
        result = call(clay_fs_tool_glob, "{\"pattern\":\"*\",\"path\":\"race\"}");
        if (ok(result)) {
            output = clay_json_string_value(clay_json_object_get(result, "output"));
            assert(!strstr(output, "sensitive-unique"));
        }
        clay_json_free(result);
    }
    int status;
    assert(waitpid(glob_child, &status, 0) == glob_child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
#else
    printf("renameat2 unavailable; race test skipped\n");
#endif
#if defined(SYS_renameat2)
    if (exchange == 0) {
        char target[512];
        snprintf(target, sizeof(target), "%s/mutate", outside);
        FILE *sentinel = fopen(target, "w");
        assert(sentinel && fputs("outside-secret", sentinel) >= 0 && fclose(sentinel) == 0);
        pid_t writer = fork();
        assert(writer >= 0);
        if (!writer) {
            for (int i = 0; i < 30000; i++)
                if (syscall(SYS_renameat2, AT_FDCWD, "race", AT_FDCWD, "race-link", RENAME_EXCHANGE))
                    _exit(1);
            _exit(0);
        }
        for (int i = 0; i < 1000; i++) {
            result = call(clay_fs_tool_write, "{\"path\":\"race/mutate\",\"content\":\"inside\"}");
            clay_json_free(result);
            result = call(clay_fs_tool_edit, "{\"path\":\"race/mutate\",\"oldString\":\"inside\",\"newString\":\"changed\"}");
            clay_json_free(result);
        }
        int status;
        assert(waitpid(writer, &status, 0) == writer && WIFEXITED(status) && !WEXITSTATUS(status));
        char buffer[32] = {0};
        sentinel = fopen(target, "r");
        assert(sentinel && fread(buffer, 1, sizeof(buffer)-1, sentinel) == strlen("outside-secret"));
        assert(fclose(sentinel) == 0 && !strcmp(buffer, "outside-secret"));
        assert(unlink(target) == 0);
        unlink("race/mutate");
    }
#endif
    assert(unlink("race-link") == 0);
    assert(unlink("race/file") == 0);
    assert(rmdir("race") == 0);
    assert(unlink(secret) == 0);
    assert(unlink(secret_name) == 0);
    assert(rmdir(outside) == 0);
#endif

#ifdef __linux__
    /* Replace the checked directory before the walker opens it for enumeration. */
    assert(mkdir("glob-swap", 0700) == 0);
    assert(mkdir("glob-swap/target", 0700) == 0);
    assert(mkdir("glob-swap-replacement", 0700) == 0);
    FILE *swap_file = fopen("glob-swap-replacement/unexpected", "w");
    assert(swap_file && fclose(swap_file) == 0);
    assert(stat("glob-swap/target", &swap_target) == 0);
    ClayArray swap_matches;
    clay_array_init(&swap_matches, sizeof(char *));
    int swap_truncated = 0;
    swap_armed = 1;
    clay_fs_walk_files("glob-swap", "", "*", &swap_matches, &swap_truncated);
    assert(swap_done && !swap_truncated && swap_matches.count == 0);
    clay_array_free(&swap_matches);
    assert(unlink("glob-swap/target/unexpected") == 0);
    assert(rmdir("glob-swap/target") == 0);
    assert(rmdir("glob-swap/old") == 0);
    assert(rmdir("glob-swap") == 0);

    /* Apply the limit to the depth-first readdir order, not root files first. */
    assert(mkdir("glob-order", 0700) == 0);
    for (int i = 0; i < 501; i++) {
        char path[64];
        snprintf(path, sizeof(path), "glob-order/dir-%03d", i);
        assert(mkdir(path, 0700) == 0);
        snprintf(path, sizeof(path), "glob-order/dir-%03d/source.c", i);
        FILE *f = fopen(path, "w"); assert(f); assert(fclose(f) == 0);
        snprintf(path, sizeof(path), "glob-order/file-%03d", i);
        f = fopen(path, "w"); assert(f); assert(fclose(f) == 0);
    }
    DIR *order_dir = opendir("glob-order");
    assert(order_dir);
    char expected[500][64];
    int count = 0, sources = 0;
    struct dirent *order_entry;
    while (count < 500 && (order_entry = readdir(order_dir))) {
        if (strncmp(order_entry->d_name, "dir-", 4) == 0) {
            snprintf(expected[count++], sizeof(expected[0]), "%.7s/source.c", order_entry->d_name);
            sources++;
        } else if (strncmp(order_entry->d_name, "file-", 5) == 0) {
            snprintf(expected[count++], sizeof(expected[0]), "%.8s", order_entry->d_name);
        }
    }
    assert(closedir(order_dir) == 0);
    assert(count == 500 && sources > 0);
    ClayArray ordered_matches;
    clay_array_init(&ordered_matches, sizeof(char *));
    int truncated = 0;
    clay_fs_walk_files("glob-order", "", "*", &ordered_matches, &truncated);
    assert(truncated && ordered_matches.count == 500);
    for (int i = 0; i < count; i++) {
        char *match = *(char **)clay_array_get(&ordered_matches, i);
        assert(strcmp(match, expected[i]) == 0);
        free(match);
    }
    clay_array_free(&ordered_matches);
    for (int i = 0; i < 501; i++) {
        char path[64];
        snprintf(path, sizeof(path), "glob-order/dir-%03d/source.c", i); assert(unlink(path) == 0);
        snprintf(path, sizeof(path), "glob-order/dir-%03d", i); assert(rmdir(path) == 0);
        snprintf(path, sizeof(path), "glob-order/file-%03d", i); assert(unlink(path) == 0);
    }
    assert(rmdir("glob-order") == 0);
#endif
    struct stat st;
    assert(chmod("a.txt", 0755) == 0);
    result = call(clay_fs_tool_write, "{\"path\":\"a.txt\",\"content\":\"line1\\nline2\\n\"}");
    assert(ok(result));
    clay_json_free(result);
    assert(stat("a.txt", &st) == 0 && (st.st_mode & 0777) == 0755);

    /* Parent directories are created automatically. */
    result = call(clay_fs_tool_write, "{\"path\":\"sub/dir/b.c\",\"content\":\"int main(){return 0;}\\n\"}");
    assert(ok(result));
    clay_json_free(result);

    /* A path escaping the workspace is rejected. */
    result = call(clay_fs_tool_read, "{\"path\":\"../outside\"}");
    assert(!ok(result));
    clay_json_free(result);
    result = call(clay_fs_tool_write, "{\"path\":\"../outside.txt\",\"content\":\"x\"}");
    assert(!ok(result));
    clay_json_free(result);

    assert(symlink("/tmp", "escape") == 0);
    result = call(clay_fs_tool_read, "{\"path\":\"escape/secret\"}");
    assert(!ok(result)); clay_json_free(result);
    result = call(clay_fs_tool_write, "{\"path\":\"escape/clay-escape-test/new\",\"content\":\"x\"}");
    assert(!ok(result)); clay_json_free(result);
    result = call(clay_fs_tool_edit, "{\"path\":\"escape/secret\",\"oldString\":\"x\"}");
    assert(!ok(result)); clay_json_free(result);
    result = call(clay_fs_tool_glob, "{\"pattern\":\"*\",\"path\":\"escape\"}");
    assert(!ok(result)); clay_json_free(result);
    assert(symlink("/tmp/no-such-clay-target", "dangling") == 0);
    result = call(clay_fs_tool_write, "{\"path\":\"dangling\",\"content\":\"x\"}");
    assert(!ok(result)); clay_json_free(result);
    unlink("dangling");
    ClayCommands commands = {0};
    ClayJson *grep_args = clay_json_parse("{\"pattern\":\"secret\",\"path\":\"escape\"}", NULL);
    result = clay_fs_tool_grep(grep_args, &commands);
    assert(!ok(result));
    clay_json_free(result);
    clay_json_free(grep_args);
    grep_args = clay_json_parse("{\"pattern\":\"secret\",\"path\":\"../\"}", NULL);
    result = clay_fs_tool_grep(grep_args, &commands);
    assert(!ok(result)); clay_json_free(result); clay_json_free(grep_args);

    assert(setenv("HOME", workspace, 1) == 0);
    commands.chat = clay_chat_create("test");
    assert(commands.chat);
    commands.sandbox_mode = CLAY_SANDBOX_MODE_UNLEASHED;
    grep_args = clay_json_parse("{\"pattern\":\"line1\",\"path\":\"a.txt\"}", NULL);
    result = clay_fs_tool_grep(grep_args, &commands);
    assert(ok(result));
    assert(strstr(clay_json_string_value(clay_json_object_get(result, "output")), "1:line1"));
    clay_json_free(result);
#ifdef __linux__
    test_grep_race(&commands);
#endif

    /* Recursive grep must not follow links outside the workspace. */
    char outside_template[] = "/tmp/clay_grep_outside_XXXXXX";
    char *outside_dir = mkdtemp(outside_template);
    assert(outside_dir);
    char outside_file[256], outside_nested[256];
    snprintf(outside_file, sizeof(outside_file), "%s/file", outside_dir);
    snprintf(outside_nested, sizeof(outside_nested), "%s/nested", outside_dir);
    FILE *fixture = fopen(outside_file, "w");
    assert(fixture && fputs("grep_marker_external_file\n", fixture) >= 0 && fclose(fixture) == 0);
    fixture = fopen(outside_nested, "w");
    assert(fixture && fputs("grep_marker_external_directory\n", fixture) >= 0 && fclose(fixture) == 0);
    assert(mkdir("grep-dir", 0700) == 0);
    fixture = fopen("grep-dir/ordinary", "w");
    assert(fixture && fputs("grep_marker_ordinary\n", fixture) >= 0 && fclose(fixture) == 0);
    assert(symlink(outside_file, "grep-dir/external-file") == 0);
    assert(symlink(outside_dir, "grep-dir/external-directory") == 0);
    ClayJson *directory_args = clay_json_parse("{\"pattern\":\"grep_marker\",\"path\":\"grep-dir\"}", NULL);
    result = clay_fs_tool_grep(directory_args, &commands);
    assert(ok(result));
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(output && strstr(output, "grep_marker_ordinary"));
    assert(!strstr(output, "grep_marker_external_file"));
    assert(!strstr(output, "grep_marker_external_directory"));
    clay_json_free(result); clay_json_free(directory_args);
    assert(unlink("grep-dir/external-file") == 0);
    assert(unlink("grep-dir/external-directory") == 0);
    assert(unlink("grep-dir/ordinary") == 0 && rmdir("grep-dir") == 0);
    assert(unlink(outside_file) == 0 && unlink(outside_nested) == 0 && rmdir(outside_dir) == 0);
    ClayJson *regex_args = clay_json_parse("{\"pattern\":\"line(1|2)+\",\"path\":\"a.txt\"}", NULL);
    result = clay_fs_tool_grep(regex_args, &commands);
    assert(ok(result));
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "1:line1"));
    assert(strstr(output, "2:line2"));
    clay_json_free(result);
    clay_json_free(regex_args);
    /* Characterize grep without depending on traversal order. */
    const char *grep_cases[][3] = {
        {"{\"pattern\":\"line[12]\",\"path\":\"a.txt\"}", "1:line1\n2:line2\n", "1"},
        {"{\"pattern\":\"not-present\",\"path\":\"a.txt\"}", "", "1"},
        {"{\"pattern\":\"[\",\"path\":\"a.txt\"}", NULL, "0"},
        {"{\"pattern\":\"x\",\"path\":\"missing-grep-file\"}", NULL, "0"},
        {"{\"pattern\":\"\",\"path\":\"a.txt\"}", NULL, "0"},
        {"{\"pattern\":\"line1\",\"path\":\"a.txt\",\"glob\":\"*.c\"}", "", "1"},
        {"{\"pattern\":\"line1\",\"path\":\"a.txt\",\"glob\":\"*.txt\"}", "1:line1\n", "1"},
        {"{\"pattern\":\"[\",\"path\":\"a.txt\",\"glob\":\"*.c\"}", NULL, "0"},
        {"{\"pattern\":\"line1\",\"path\":\"a.txt\",\"glob\":\"[ab].txt\"}", "1:line1\n", "1"},
        {"{\"pattern\":\"line1\",\"path\":\"a.txt\",\"glob\":\"x/*.txt\"}", "", "1"},
        {"{\"pattern\":\"line1' ; echo INJECTED; #\",\"path\":\"a.txt\"}", "", "1"},
    };
    for (size_t i = 0; i < sizeof(grep_cases) / sizeof(grep_cases[0]); i++) {
        ClayJson *args = clay_json_parse(grep_cases[i][0], NULL);
        result = clay_fs_tool_grep(args, &commands);
        assert(ok(result) == (grep_cases[i][2][0] == '1'));
        output = clay_json_string_value(clay_json_object_get(result, "output"));
        if (grep_cases[i][1] && (!output || strcmp(output, grep_cases[i][1]) != 0)) fprintf(stderr, "case %zu output=[%s] expected=[%s]\n", i, output ? output : "NULL", grep_cases[i][1]);
        if (grep_cases[i][1]) assert(output && strcmp(output, grep_cases[i][1]) == 0);
        if (!ok(result)) assert(clay_json_string_value(clay_json_object_get(result, "error")));
        clay_json_free(result); clay_json_free(args);
    }
    FILE *grep_file = fopen("grep-binary", "wb"); assert(grep_file);
    assert(fwrite("line1\0binary\n", 1, 13, grep_file) == 13); assert(fclose(grep_file) == 0);
    ClayJson *binary_args = clay_json_parse("{\"pattern\":\"line1\",\"path\":\"grep-binary\"}", NULL);
    result = clay_fs_tool_grep(binary_args, &commands);
    assert(ok(result)); assert(strcmp(clay_json_string_value(clay_json_object_get(result, "output")), "") == 0);
    clay_json_free(result); clay_json_free(binary_args); assert(unlink("grep-binary") == 0);
    grep_file = fopen("grep-large", "w"); assert(grep_file);
    for (int i = 0; i < 10000; i++) assert(fputs("line1 long output\n", grep_file) >= 0);
    assert(fclose(grep_file) == 0);
    ClayJson *large_args = clay_json_parse("{\"pattern\":\"line1\",\"path\":\"grep-large\"}", NULL);
    result = clay_fs_tool_grep(large_args, &commands);
    assert(ok(result)); assert(clay_json_bool_value(clay_json_object_get(result, "output_truncated")));
    assert(clay_json_string_value(clay_json_object_get(result, "scratch_path")));
    clay_json_free(result); clay_json_free(large_args); assert(unlink("grep-large") == 0);
#ifdef __linux__
    test_grep_directory_race_baseline(&commands);
    /* Swap the checked start directory immediately before grep opens it. */
    if (access("/usr/bin/grep", X_OK) == 0) {
    char outside_template[] = "/tmp/clay_grep_outside_XXXXXX";
    char *race_outside = mkdtemp(outside_template); assert(race_outside);
    char outside_file[512]; snprintf(outside_file, sizeof(outside_file), "%s/secret", race_outside);
    FILE *file = fopen(outside_file, "w"); assert(file); assert(fputs("race-secret\n", file) >= 0); assert(fclose(file) == 0);
    assert(mkdir("grep-race", 0700) == 0); assert(mkdir("grep-bin", 0700) == 0);
    file = fopen("grep-bin/grep", "w"); assert(file);
    assert(fputs("#!/bin/sh\n/bin/mv grep-race grep-race-saved\n/bin/ln -s \"$CLAY_GREP_OUTSIDE\" grep-race\nexec /usr/bin/grep \"$@\"\n", file) >= 0);
    assert(fclose(file) == 0); assert(chmod("grep-bin/grep", 0700) == 0);
    const char *old_path_env = getenv("PATH"); char *old_path = old_path_env ? strdup(old_path_env) : NULL;
    ClayStr race_path; clay_str_init(&race_path); clay_str_printf(&race_path, "%s/grep-bin:%s", workspace, old_path ? old_path : "");
    assert(setenv("PATH", race_path.data, 1) == 0); assert(setenv("CLAY_GREP_OUTSIDE", race_outside, 1) == 0);
    ClayJson *race_args = clay_json_parse("{\"pattern\":\"race-secret\",\"path\":\"grep-race\"}", NULL);
    result = clay_fs_tool_grep(race_args, &commands);
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    printf("grep race baseline: %s\n", output && strstr(output, "race-secret") ? "outside content exposed" : "outside content not observed");
    if (getenv("CLAY_ASSERT_GREP_RACE_SAFETY")) {
        struct stat swapped;
        assert(lstat("grep-race", &swapped) == 0 && S_ISLNK(swapped.st_mode));
        assert(!output || !strstr(output, "race-secret"));
    }
    clay_json_free(result); clay_json_free(race_args);
    if (old_path) { assert(setenv("PATH", old_path, 1) == 0); free(old_path); } else assert(unsetenv("PATH") == 0);
    assert(unsetenv("CLAY_GREP_OUTSIDE") == 0); clay_str_free(&race_path);
    struct stat race_stat; assert(lstat("grep-race", &race_stat) == 0);
    if (S_ISLNK(race_stat.st_mode)) { assert(unlink("grep-race") == 0); assert(rmdir("grep-race-saved") == 0); }
    else assert(rmdir("grep-race") == 0);
    assert(unlink("grep-bin/grep") == 0); assert(rmdir("grep-bin") == 0);
    assert(unlink(outside_file) == 0); assert(rmdir(race_outside) == 0);
    } else printf("grep race baseline: /usr/bin/grep unavailable; skipped\n");
#endif
    ClayJson *error_args = clay_json_parse("{\"pattern\":\"line1\",\"path\":\"missing.txt\"}", NULL);
    result = clay_fs_tool_grep(error_args, &commands);
    assert(!ok(result));
    assert(strstr(clay_json_string_value(clay_json_object_get(result, "error")), "grep failed"));
    clay_json_free(result); clay_json_free(error_args);
    char *scratch = clay_chat_scratch_dir(commands.chat);
    ClaySandboxConfig probe = {.mode = CLAY_SANDBOX_MODE_SANDBOX, .workspace_dir = workspace, .scratch_dir = scratch};
    ClayStr probe_output; clay_str_init(&probe_output);
    ClayExecResult probe_result = {0};
    int probe_rc = clay_sandbox_exec(&probe, "test -f /workspace/a.txt", &probe_output, 4096, NULL, &probe_result);
    if (probe_rc == 0 && probe_result.exit_code == 0) {
        commands.sandbox_mode = CLAY_SANDBOX_MODE_SANDBOX;
#ifdef __linux__
        test_grep_race(&commands);
        test_grep_directory_race_baseline(&commands);
#endif
        result = clay_fs_tool_grep(grep_args, &commands);
        assert(ok(result));
        assert(strstr(clay_json_string_value(clay_json_object_get(result, "output")), "1:line1"));
        clay_json_free(result);
    } else printf("sandbox grep integration unavailable; skipped\n");
    clay_str_free(&probe_output); free(scratch);
    clay_json_free(grep_args); clay_chat_destroy(commands.chat);
    assert(symlink(".", "loop") == 0);
    assert(symlink("a.txt", "file-link") == 0);
    assert(symlink("missing", "dangling-link") == 0);
    result = call(clay_fs_tool_glob, "{\"pattern\":\"*\"}");
    assert(ok(result));
    assert(!strstr(clay_json_string_value(clay_json_object_get(result, "output")), "escape"));
    assert(!strstr(clay_json_string_value(clay_json_object_get(result, "output")), "loop"));
    assert(!strstr(clay_json_string_value(clay_json_object_get(result, "output")), "file-link"));
    assert(!strstr(clay_json_string_value(clay_json_object_get(result, "output")), "dangling-link"));
    assert(strstr(clay_json_string_value(clay_json_object_get(result, "output")), "a.txt"));
    assert(strstr(clay_json_string_value(clay_json_object_get(result, "output")), "sub/dir/b.c"));
    clay_json_free(result);
    unlink("file-link");
    unlink("dangling-link");
    unlink("loop");
    unlink("escape");
    /* A nonexistent file suggests the real one by substring match. */
    result = call(clay_fs_tool_read, "{\"path\":\"a.tx\"}");
    assert(!ok(result));
    const char *error = clay_json_string_value(clay_json_object_get(result, "error"));
    assert(strstr(error, "did you mean"));
    assert(strstr(error, "a.txt"));
    clay_json_free(result);

    result = call(clay_fs_tool_glob, "{\"pattern\":\"*.txt\"}");
    assert(ok(result));
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "a.txt"));
    assert(!strstr(output, "b.c"));
    clay_json_free(result);

    result = call(clay_fs_tool_glob, "{\"pattern\":\"*.c\"}");
    assert(ok(result));
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "sub/dir/b.c"));
    clay_json_free(result);

#ifdef __linux__
    inject_create_change = 1;
    result = call(clay_fs_tool_write, "{\"path\":\"create-race\",\"content\":\"written\"}");
    assert(!ok(result) && !inject_create_change); clay_json_free(result);
    char create_content[9] = {0};
    int create_fd = open("create-race", O_RDONLY);
    assert(create_fd >= 0 && read(create_fd, create_content, 8) == 8);
    assert(!strcmp(create_content, "external") && close(create_fd) == 0);
    assert(unlink("create-race") == 0);
    DIR *create_dir = opendir("."); assert(create_dir);
    struct dirent *create_entry;
    while ((create_entry = readdir(create_dir))) assert(strncmp(create_entry->d_name, ".clay-", 6));
    assert(closedir(create_dir) == 0);
    result = call(clay_fs_tool_write, "{\"path\":\"commit-race\",\"content\":\"before\"}");
    assert(ok(result)); clay_json_free(result);
    inject_commit_change = 1;
    result = call(clay_fs_tool_edit, "{\"path\":\"commit-race\",\"oldString\":\"before\",\"newString\":\"edited\"}");
    assert(ok(result) && !inject_commit_change); clay_json_free(result);
    char race_content[16] = {0};
    int race_fd = open("commit-race", O_RDONLY);
    assert(race_fd >= 0 && read(race_fd, race_content, sizeof(race_content)) == 6);
    assert(!strcmp(race_content, "edited") && close(race_fd) == 0);
    inject_commit_change = 1;
    result = call(clay_fs_tool_write, "{\"path\":\"commit-race\",\"content\":\"written\"}");
    assert(ok(result) && !inject_commit_change); clay_json_free(result);
    memset(race_content, 0, sizeof(race_content));
    race_fd = open("commit-race", O_RDONLY);
    assert(race_fd >= 0 && read(race_fd, race_content, sizeof(race_content)) == 7);
    assert(!strcmp(race_content, "written") && close(race_fd) == 0);
    assert(unlink("commit-race") == 0);
#endif

    /* oldString absent from the file. */
    result = call(clay_fs_tool_edit, "{\"path\":\"a.txt\",\"oldString\":\"nope\",\"newString\":\"x\"}");
    assert(!ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_edit, "{\"path\":\"a.txt\",\"oldString\":\"line1\",\"newString\":\"LINE_ONE\"}");
    assert(ok(result));
    clay_json_free(result);

    assert(stat("a.txt", &st) == 0 && (st.st_mode & 0777) == 0755);
    result = call(clay_fs_tool_read, "{\"path\":\"a.txt\"}");
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "LINE_ONE"));
    assert(!strstr(output, "\tline1\n"));
    clay_json_free(result);

    result = call(clay_fs_tool_write, "{\"path\":\"dup.txt\",\"content\":\"foo foo foo\"}");
    assert(ok(result));
    clay_json_free(result);

    /* Ambiguous oldString is rejected without replaceAll. */
    result = call(clay_fs_tool_edit, "{\"path\":\"dup.txt\",\"oldString\":\"foo\",\"newString\":\"bar\"}");
    assert(!ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_edit,
                  "{\"path\":\"dup.txt\",\"oldString\":\"foo\",\"newString\":\"bar\",\"replaceAll\":true}");
    assert(ok(result));
    clay_json_free(result);

    result = call(clay_fs_tool_read, "{\"path\":\"dup.txt\"}");
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "bar bar bar"));
    clay_json_free(result);

    remove("a.txt");
    remove("dup.txt");
    remove("sub/dir/b.c");
    rmdir("sub/dir");
    rmdir("sub");
    assert(chdir("/tmp") == 0);
    rmdir(workspace);

    printf("fs_tools tests passed\n");
    return 0;
}
