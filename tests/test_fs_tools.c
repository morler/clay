#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    ClayJson *regex_args = clay_json_parse("{\"pattern\":\"line(1|2)+\",\"path\":\"a.txt\"}", NULL);
    result = clay_fs_tool_grep(regex_args, &commands);
    assert(ok(result));
    output = clay_json_string_value(clay_json_object_get(result, "output"));
    assert(strstr(output, "1:line1"));
    assert(strstr(output, "2:line2"));
    clay_json_free(result);
    clay_json_free(regex_args);
    char *scratch = clay_chat_scratch_dir(commands.chat);
    ClaySandboxConfig probe = {.mode = CLAY_SANDBOX_MODE_SANDBOX, .workspace_dir = workspace, .scratch_dir = scratch};
    ClayStr probe_output; clay_str_init(&probe_output);
    ClayExecResult probe_result = {0};
    int probe_rc = clay_sandbox_exec(&probe, "test -f /workspace/a.txt", &probe_output, 4096, NULL, &probe_result);
    if (probe_rc == 0 && probe_result.exit_code == 0) {
        commands.sandbox_mode = CLAY_SANDBOX_MODE_SANDBOX;
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
