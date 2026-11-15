#define _GNU_SOURCE

#include "../src/commands/context.h"

#include "clay/term.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <sys/syscall.h>

static int inject_restore, inject_delete;
static void concurrent_change(int dir, const char *name) {
    int fd = openat(dir, name, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, "external", 8) == 8 && close(fd) == 0);
}
int renameat(int olddir, const char *old, int newdir, const char *new) {
    if (inject_restore && !strcmp(new, "restore-race")) {
        inject_restore = 0;
        concurrent_change(newdir, new);
    }
    return (int)syscall(SYS_renameat, olddir, old, newdir, new);
}
int unlinkat(int dir, const char *name, int flags) {
    if (inject_delete && !strcmp(name, "delete-race")) {
        inject_delete = 0;
        concurrent_change(dir, name);
    }
    return (int)syscall(SYS_unlinkat, dir, name, flags);
}
#endif

extern char *mkdtemp(char *template);

static void write_file(const char *path, const char *content) {
    assert(clay_term_write_project_file_atomic(path, content, strlen(content)) == 0);
}

static void assert_file(const char *path, const char *expected) {
    ClayStr body;
    assert(clay_term_read_file(path, 1024, &body) == 0);
    assert(body.len == strlen(expected));
    assert(memcmp(body.data, expected, body.len) == 0);
    clay_str_free(&body);
}

int main(void) {
    char template[] = "/tmp/clay_test_undo_XXXXXX";
    char *workspace = mkdtemp(template);
    assert(workspace);
    assert(chdir(workspace) == 0);

    ClayCommands commands = {0};
    clay_array_init(&commands.undo_history, sizeof(ClayUndoEntry));

    write_file("existing.txt", "before");
    assert(chmod("existing.txt", 0755) == 0);
    assert(clay_commands_undo_prepare(&commands, "existing.txt"));
    write_file("existing.txt", "after");
    clay_commands_undo_commit(&commands);
    assert(commands.undo_history.count == 1);
    clay_cmd_undo("", &commands);
    assert_file("existing.txt", "before");
    struct stat st;
    assert(stat("existing.txt", &st) == 0 && (st.st_mode & 0777) == 0755);
    assert(commands.undo_history.count == 0);

    assert(clay_commands_undo_prepare(&commands, "new.txt"));
    write_file("new.txt", "new");
    clay_commands_undo_commit(&commands);
    clay_cmd_undo("", &commands);
    assert(access("new.txt", F_OK) != 0);

    write_file("changed.txt", "one");
    assert(clay_commands_undo_prepare(&commands, "changed.txt"));
    write_file("changed.txt", "two");
    clay_commands_undo_commit(&commands);
    write_file("changed.txt", "external");
    clay_cmd_undo("", &commands);
    assert_file("changed.txt", "external");
    assert(commands.undo_history.count == 1);

    /* In-place changes, replacement, and removal after capture conflict. */
    FILE *changed = fopen("changed.txt", "w");
    assert(changed && fputs("in-place", changed) >= 0 && fclose(changed) == 0);
    clay_cmd_undo("", &commands);
    assert_file("changed.txt", "in-place");
    assert(commands.undo_history.count == 1);

    write_file("replacement.tmp", "replacement");
    assert(rename("replacement.tmp", "changed.txt") == 0);
    clay_cmd_undo("", &commands);
    assert_file("changed.txt", "replacement");
    assert(commands.undo_history.count == 1);

    assert(remove("changed.txt") == 0);
    clay_cmd_undo("", &commands);
    assert(access("changed.txt", F_OK) != 0);
    assert(commands.undo_history.count == 1);

    /* A successful write with a missing after-state must not be recorded. */
    write_file("vanished.txt", "before");
    assert(clay_commands_undo_prepare(&commands, "vanished.txt"));
    write_file("vanished.txt", "after");
    assert(remove("vanished.txt") == 0);
    clay_commands_undo_commit(&commands);
    assert(commands.undo_history.count == 1);
    clay_cmd_undo("", &commands);
    assert(access("vanished.txt", F_OK) != 0);
    assert(mkdir("dir", 0700) == 0);
    write_file("dir/file", "before");
    assert(clay_commands_undo_prepare(&commands, "dir/file"));
    write_file("dir/file", "after");
    clay_commands_undo_commit(&commands);
    assert(rename("dir", "saved") == 0);
    assert(symlink("/tmp", "dir") == 0);
    size_t count = commands.undo_history.count;
    clay_cmd_undo("", &commands);
    assert(commands.undo_history.count == count);
    assert_file("saved/file", "after");
    assert(!clay_commands_undo_prepare(&commands, "dir/file"));
    unlink("dir"); remove("saved/file"); rmdir("saved");

#ifdef __linux__
    /* Demonstrate the documented window after undo's snapshot comparison. */
    write_file("restore-race", "before");
    assert(clay_commands_undo_prepare(&commands, "restore-race"));
    write_file("restore-race", "after");
    clay_commands_undo_commit(&commands);
    count = commands.undo_history.count;
    inject_restore = 1;
    clay_cmd_undo("", &commands);
    assert(!inject_restore && commands.undo_history.count == count - 1);
    assert_file("restore-race", "before");
    assert(unlink("restore-race") == 0);

    assert(clay_commands_undo_prepare(&commands, "delete-race"));
    write_file("delete-race", "after");
    clay_commands_undo_commit(&commands);
    count = commands.undo_history.count;
    inject_delete = 1;
    clay_cmd_undo("", &commands);
    assert(!inject_delete && commands.undo_history.count == count - 1);
    assert(access("delete-race", F_OK) != 0);
#endif
    clay_commands_undo_destroy(&commands);
    remove("existing.txt");
    remove("changed.txt");
    assert(chdir("/tmp") == 0);
    rmdir(workspace);
    printf("undo tests passed\n");
    return 0;
}
