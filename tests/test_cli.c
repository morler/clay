#include "clay/cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char *argv[] = {"clay", "command", "--flag", "--hello", "world", "-foo", "1234"};
    ClayCli *cli = clay_cli_create("clay", "test");
    clay_cli_add_bool(cli, "flag", "test flag");
    clay_cli_add_string(cli, "hello", "test string");
    clay_cli_add_number(cli, "foo", "test number");

    int rc = clay_cli_parse(cli, (int)(sizeof(argv) / sizeof(argv[0])), argv);
    int ok = rc == 0 && clay_cli_bool(cli, "flag") &&
             clay_cli_string(cli, "hello") && strcmp(clay_cli_string(cli, "hello"), "world") == 0 &&
             clay_cli_number(cli, "foo") == 1234.0 &&
             clay_cli_command(cli) && strcmp(clay_cli_command(cli), "command") == 0;
    clay_cli_destroy(cli);

    if (!ok) {
        fputs("cli test failed\n", stderr);
        return 1;
    }

    char *prompt = NULL;
    int json = 0;
    char *prompt_argv[] = {"clay", "-p", "review this"};
    if (clay_cli_startup_with_prompt(3, prompt_argv, "test", &prompt, &json) != 0 ||
        !prompt || strcmp(prompt, "review this") != 0 || json) {
        free(prompt);
        fputs("cli prompt test failed\n", stderr);
        return 1;
    }
    free(prompt);

    /* --json only means something with a one-shot prompt; alone it is a
       usage error, and together it reaches the run. */
    char *json_prompt = NULL;
    int json_flag = 0;
    char *json_argv[] = {"clay", "-p", "review this", "--json"};
    if (clay_cli_startup_with_prompt(4, json_argv, "test", &json_prompt,
                                     &json_flag) != 0 ||
        !json_prompt || !json_flag) {
        free(json_prompt);
        fputs("cli json test failed\n", stderr);
        return 1;
    }
    free(json_prompt);

    char *orphan_prompt = NULL;
    int orphan_flag = 0;
    char *orphan_argv[] = {"clay", "--json"};
    if (clay_cli_startup_with_prompt(2, orphan_argv, "test", &orphan_prompt,
                                      &orphan_flag) == 0) {
        fputs("cli json without a prompt should fail\n", stderr);
        return 1;
    }

    return 0;
}
