#include "clay/clay.h"
#include "clay/http.h"
#include "clay/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CLAY_VERSION "0.0.4"

static int require_provider(ClayCommands *commands) {
    clay_sayc(CLAY_YELLOW, "Connect a provider to continue.");
    while (!clay_commands_has_provider(commands)) {
        clay_commands_connect(commands);
        if (!clay_term_is_interactive() && !clay_commands_has_provider(commands)) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "shell") == 0)
        return clay_shell_main(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "skill") == 0)
        return clay_skill_cli_main(argc - 2, argv + 2);
    char *one_shot_prompt = NULL;
    int one_shot_json = 0;
    int cli_status = clay_cli_startup_with_prompt(argc, argv, CLAY_VERSION,
                                                  &one_shot_prompt,
                                                  &one_shot_json);
    if (cli_status != 0) return cli_status < 0 ? 1 : 0;

    clay_term_init();
    if (one_shot_prompt) {
        clay_term_set_noninteractive(1);
        clay_term_set_color_enabled(0);

    }
    if (clay_http_init() != 0) {
        fprintf(stderr, "Failed to initialize HTTP.\n");
        return 1;
    }
    if (!one_shot_prompt) clay_banner(CLAY_VERSION);

    ClayApp *app = clay_app_create();
    ClayCommands *commands = clay_commands_create(app);
    if (!commands) {
        fprintf(stderr, "Error: could not create command session.\n");
        clay_app_destroy(app);
        return 1;
    }
    clay_commands_register(commands);
    if (!clay_commands_has_provider(commands) &&
        (one_shot_prompt || require_provider(commands) != 0)) {
        if (one_shot_prompt)
            fprintf(stderr, "Error: no provider is configured for --prompt. "
                            "Set provider credentials in the environment or "
                            "run /connect interactively.\n");
        clay_commands_destroy(commands);
        clay_app_destroy(app);
        clay_http_cleanup();
        free(one_shot_prompt);
        return 1;
    }

    if (one_shot_prompt) {
        /* --json owns stdout: the run's own tool chatter is written to a
           scratch stream and dropped, so the only line left is the summary. */
        int saved_stdout = -1;
        if (one_shot_json) {
            fflush(stdout);
            saved_stdout = dup(STDOUT_FILENO);
            if (saved_stdout < 0)
                fprintf(stderr, "clay: could not keep stdout for --json\n");
            FILE *sink = fopen("/dev/null", "w");
            if (sink) {
                dup2(fileno(sink), STDOUT_FILENO);
                fclose(sink);
            }
        }
        struct timespec started, finished;
        clock_gettime(CLOCK_MONOTONIC, &started);
        int ok = clay_commands_run_message(commands, one_shot_prompt);
        clock_gettime(CLOCK_MONOTONIC, &finished);
        long long duration_ms =
            (long long)(finished.tv_sec - started.tv_sec) * 1000 +
            (finished.tv_nsec - started.tv_nsec) / 1000000;
        free(one_shot_prompt);
        if (one_shot_json) {
            fflush(stdout);
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
            clay_commands_print_json_summary(commands, duration_ms);
        }
        clay_commands_destroy(commands);
        clay_app_destroy(app);
        clay_http_cleanup();
        return ok ? 0 : 1;
    }

    int interrupted = 0;
    while (clay_commands_running(commands)) {
        char *line = clay_prompt_line(clay_app_commands(app));
        if (!line) {
            interrupted = clay_prompt_was_interrupted() || clay_term_take_interrupt();
            break;
        }

        ClayInput input = clay_input_parse(line);
        free(line);
        int prompt_ready = 0;
        if (input.kind == CLAY_INPUT_COMMAND) {
            if (!clay_command_dispatch(clay_app_commands(app), &input)) {
                clay_sayc(CLAY_RED, "Unknown command: /%s (try /help)", input.command);
            }
        } else if (input.kind == CLAY_INPUT_MESSAGE) {
            prompt_ready = clay_commands_run_message(commands, input.raw);
            clay_term_notify("Clay", "Turn finished");
        }
        clay_input_free(&input);
        if (clay_term_take_interrupt()) {
            interrupted = 1;
            break;
        }
        if (!prompt_ready) fputc('\n', stdout);
    }

    if (interrupted) clay_commands_print_session_summary(commands);
    clay_commands_destroy(commands);
    clay_app_destroy(app);
    clay_http_cleanup();
    if (!interrupted) printf("%sGoodbye.%s\n", clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
    return 0;
}
