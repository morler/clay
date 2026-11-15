#include "clay/sandbox.h"
#include "clay/str.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run(ClaySandboxMode mode, const char *workspace, const char *scratch, const char *command, ClayStr *output,
               int *exit_code) {
    ClaySandboxConfig config = {.mode = mode, .workspace_dir = workspace, .scratch_dir = scratch};
    ClayExecResult exec = {0};
    clay_str_clear(output);
    int rc = clay_sandbox_exec(&config, command, output, 64 * 1024, NULL, &exec);
    *exit_code = exec.exit_code;
    return rc;
}

static void collect(const char *data, size_t len, void *userdata) {
    clay_str_push_n(userdata, data, len);
}

static void test_binary_capture(ClaySandboxMode mode, const char *workspace, const char *scratch) {
    ClaySandboxConfig config = {.mode = mode, .workspace_dir = workspace, .scratch_dir = scratch};
    ClayStr output, streamed;
    clay_str_init(&output);
    clay_str_init(&streamed);
    ClayExecResult result = {0};
    ClayExecOptions options = {.on_output = collect, .user_data = &streamed, .preserve_output_nul = 1};
    assert(clay_sandbox_exec(&config, "printf 'a\\000b'", &output, 2, &options, &result) == 0);
    assert(result.exit_code == 0 && result.output_truncated);
    assert(output.len == 2 && memcmp(output.data, "a\0", 2) == 0);
    assert(streamed.len == 3 && memcmp(streamed.data, "a\0b", 3) == 0);
    clay_str_clear(&output);
    clay_str_clear(&streamed);
    assert(clay_sandbox_exec(&config, "printf 'a\\000b'", &output, 10, &options, &result) == 0);
    assert(result.exit_code == 0 && !result.output_truncated);
    assert(output.len == 3 && memcmp(output.data, "a\0b", 3) == 0);
    assert(streamed.len == 3 && memcmp(streamed.data, "a\0b", 3) == 0);
    clay_str_clear(&output);
    clay_str_clear(&streamed);
    options.preserve_output_nul = 0;
    assert(clay_sandbox_exec(&config, "printf 'a\\000b'", &output, 10, &options, &result) == 0);
    assert(result.exit_code == 0 && !result.output_truncated);
    assert(output.len == 3 && memcmp(output.data, "a?b", 3) == 0);
    assert(streamed.len == 3 && memcmp(streamed.data, "a?b", 3) == 0);
    clay_str_free(&output);
    clay_str_free(&streamed);
}
int main(void) {
    if (!clay_sandbox_supported()) {
        printf("sandbox not supported on this platform, skipping\n");
        return 0;
    }

    char workspace_template[] = "/tmp/clay_test_workspace_XXXXXX";
    char scratch_template[] = "/tmp/clay_test_scratch_XXXXXX";
    char *workspace = mkdtemp(workspace_template);
    char *scratch = mkdtemp(scratch_template);
    assert(workspace && scratch);
    test_binary_capture(CLAY_SANDBOX_MODE_UNLEASHED, workspace, scratch);

    ClayStr output;
    clay_str_init(&output);
    int exit_code = -1;

    char path[] = "/tmp/clay_input_XXXXXX";
    int input = mkstemp(path);
    assert(input >= 0 && write(input, "input-ok\n", 9) == 9);
    close(input);
    input = open(path, O_RDONLY | O_CLOEXEC);
    assert(input >= 0);
    unlink(path);
    ClaySandboxConfig config = {.mode = CLAY_SANDBOX_MODE_UNLEASHED,
                                .workspace_dir = workspace, .scratch_dir = scratch};
    ClayExecResult exec;
    assert(clay_sandbox_exec_input(&config, "cat", &output, 65536, NULL, &exec, input) == 0);
    assert(exec.exit_code == 0 && strcmp(output.data, "input-ok\n") == 0);
    assert(fcntl(input, F_GETFD) & FD_CLOEXEC);
    assert(lseek(input, 0, SEEK_SET) == 0);
    clay_str_clear(&output);
    assert(clay_term_shell_exec_input("cat", &output, 65536, NULL, &exec, input) == 0);
    assert(exec.exit_code == 0 && strcmp(output.data, "input-ok\n") == 0);
    int saved = dup(STDIN_FILENO);
    assert(saved >= 0 && dup2(input, STDIN_FILENO) == STDIN_FILENO);
    assert(fcntl(STDIN_FILENO, F_SETFD, FD_CLOEXEC) == 0);
    assert(lseek(input, 0, SEEK_SET) == 0);
    clay_str_clear(&output);
    assert(clay_term_shell_exec_input("cat", &output, 65536, NULL, &exec, 0) == 0);
    assert(exec.exit_code == 0 && strcmp(output.data, "input-ok\n") == 0);
    assert(fcntl(STDIN_FILENO, F_GETFD) & FD_CLOEXEC);
    clay_str_clear(&output);
    assert(clay_sandbox_exec(&config, "cat", &output, 65536, NULL, &exec) == 0);
    assert(exec.exit_code != 0);
    assert(fcntl(STDIN_FILENO, F_SETFD, 0) == 0);
    assert(lseek(input, 0, SEEK_SET) == 0);
    clay_str_clear(&output);
    assert(clay_sandbox_exec(&config, "cat", &output, 65536, NULL, &exec) == 0);
    assert(exec.exit_code == 0 && strcmp(output.data, "input-ok\n") == 0);
    assert(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
    close(saved);
    int invalid = dup(input);
    assert(invalid >= 0);
    close(invalid);
    clay_str_clear(&output);
    assert(clay_sandbox_exec_input(&config, "echo must-not-run", &output, 65536, NULL, &exec, invalid) == -1);
    assert(output.len == 0);
    assert(lseek(input, 0, SEEK_SET) == 0);
    config.mode = CLAY_SANDBOX_MODE_SANDBOX;
    assert(clay_sandbox_exec_input(&config, "cat; echo sandbox-ran", &output, 65536, NULL, &exec, input) == 0);
    if (exec.exit_code == 126) {
        assert(!output.data || strstr(output.data, "sandbox-ran") == NULL);
    } else {
        assert(exec.exit_code == 0);
        assert(strstr(output.data, "input-ok\n") && strstr(output.data, "sandbox-ran"));
    }
    config.workspace_dir = "/this-workspace-does-not-exist-clay-test";
    clay_str_clear(&output);
    assert(clay_sandbox_exec_input(&config, "echo must-not-run", &output, 65536, NULL, &exec, input) == 0);
    assert(exec.exit_code == 126);
    assert(!output.data || strstr(output.data, "must-not-run") == NULL);
    close(input);
    clay_str_clear(&output);

    int rc = run(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch, "echo hi > /workspace/marker && cat /workspace/marker",
                 &output, &exit_code);
    if (rc != 0 || exit_code == 126) {
        printf("sandbox unavailable on this host, skipping\n");
        clay_str_free(&output);
        return 0;
    }
    assert(rc == 0);
    assert(exit_code == 0);
    assert(strstr(output.data, "hi") != NULL);
    test_binary_capture(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch);

    ClayStr marker_path;
    clay_str_init(&marker_path);
    clay_str_printf(&marker_path, "%s/marker", workspace);
    FILE *marker = fopen(marker_path.data, "r");
    assert(marker != NULL);
    fclose(marker);
    clay_str_free(&marker_path);

    const char *scratch_name = strrchr(scratch, '/');
    scratch_name = scratch_name ? scratch_name + 1 : scratch;
    ClayStr scratch_command;
    clay_str_init(&scratch_command);
    clay_str_printf(&scratch_command,
                    "echo scratch-ok > /scratch/note && test -f /tmp/%s/note && cat /tmp/%s/note && "
                    "test \"$TMPDIR\" = /tmp/%s",
                    scratch_name, scratch_name, scratch_name);
    assert(run(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch, scratch_command.data, &output, &exit_code) == 0);
    clay_str_free(&scratch_command);
    assert(exit_code == 0);
    assert(strstr(output.data, "scratch-ok") != NULL);

    assert(run(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch,
               "test ! -e /home && test \"$HOME\" = /scratch && test -z \"$OPENAI_API_KEY\"", &output, &exit_code) == 0);
    assert(exit_code == 0);

    assert(run(CLAY_SANDBOX_MODE_SANDBOX, workspace, scratch,
               "test \"$(ulimit -u)\" -le 64 && test \"$(ulimit -v)\" -le 1048576", &output, &exit_code) == 0);
    assert(exit_code == 0);

    assert(run(CLAY_SANDBOX_MODE_UNLEASHED, workspace, scratch, "echo unleashed-ok", &output, &exit_code) == 0);
    assert(exit_code == 0);
    assert(strstr(output.data, "unleashed-ok") != NULL);

    clay_str_free(&output);
    printf("sandbox tests passed\n");
    return 0;
}
