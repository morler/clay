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
#endif

#ifdef __linux__
static struct stat swap_target;
static int swap_armed, swap_done;
static int grep_swap_armed, grep_swap_done;
static const char *grep_swap_secret;
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

int main(void) {
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
#if defined(SYS_openat2)
    pid_t filtered = fork();
    assert(filtered >= 0);
    if (filtered == 0) {
        struct sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat2, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog = { .len = sizeof(filter) / sizeof(filter[0]), .filter = filter };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) _exit(77);
        ClayJson *r = call(clay_fs_tool_read, "{\"path\":\"a.txt\"}");
        int success = ok(r);
        clay_json_free(r);
        r = call(clay_fs_tool_read, "{\"path\":\"a.tx\"}");
        const char *message = clay_json_string_value(clay_json_object_get(r, "error"));
        success = success && !ok(r) && message && strstr(message, "a.txt") && !strstr(message, "a.tx-link");
        clay_json_free(r);
        r = call(clay_fs_tool_glob, "{\"pattern\":\"*\"}");
        const char *listing = clay_json_string_value(clay_json_object_get(r, "output"));
        success = success && ok(r) && listing && strstr(listing, "a.txt") && !strstr(listing, "a.tx-link");
        clay_json_free(r);
        _exit(success ? 0 : 1);
    }
    int filtered_status;
    assert(waitpid(filtered, &filtered_status, 0) == filtered);
    assert(WIFEXITED(filtered_status));
    if (WEXITSTATUS(filtered_status) == 77) printf("seccomp unavailable; skipped\n");
    else assert(WEXITSTATUS(filtered_status) == 0);
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
    assert(fchdir(cwd) == 0);
    struct rlimit old_limit, low_limit;
    assert(getrlimit(RLIMIT_NOFILE, &old_limit) == 0);
    low_limit = old_limit;
    if (low_limit.rlim_cur > 32) low_limit.rlim_cur = 32;
    assert(setrlimit(RLIMIT_NOFILE, &low_limit) == 0);
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
