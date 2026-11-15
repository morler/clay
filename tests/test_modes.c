#define _GNU_SOURCE

#include "../src/commands/context.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *mkdtemp(char *template);

static void assert_mode(ClayCommands *commands, ClaySandboxMode mode, int auto_approve,
                        const char *persisted) {
    assert(commands->sandbox_mode == mode);
    assert(commands->sandbox_auto_approve == auto_approve);
    char *saved = clay_config_sandbox_mode();
    assert(strcmp(saved, persisted) == 0);
    free(saved);
}

static void test_provider_selection(void) {
    ClayProviderConfig custom = {
        .id = "custom", .apikey = "custom-key", .base_url = "https://example.com/v1"
    };
    assert(clay_config_save(&custom) == 0);
    assert(clay_config_selection_save("custom", "saved-model") == 0);
    assert(setenv("OPENAI_API_KEY", "openai-key", 1) == 0);
    assert(unsetenv("CLAY_PROVIDER") == 0);

    ClayCommands *commands = clay_commands_create(NULL);
    assert(commands);
    assert(commands->selected_provider && strcmp(commands->selected_provider, "custom") == 0);
    assert(commands->selected_model && strcmp(commands->selected_model, "saved-model") == 0);
    assert(!clay_commands_reasoning_effort(commands)->id);
    clay_commands_destroy(commands);

    assert(setenv("CLAY_PROVIDER", "openai", 1) == 0);
    commands = clay_commands_create(NULL);
    assert(commands);
    assert(commands->selected_provider && strcmp(commands->selected_provider, "openai") == 0);
    assert(!commands->selected_model);
    clay_commands_destroy(commands);

    char *provider = NULL, *model = NULL;
    assert(clay_config_selection_load(&provider, &model) == 0);
    assert(provider && strcmp(provider, "custom") == 0);
    assert(model && strcmp(model, "saved-model") == 0);
    free(provider);
    free(model);
}

int main(void) {
    char template[] = "/tmp/clay_test_modes_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(clearenv() == 0);
    assert(setenv("HOME", home, 1) == 0);
    test_provider_selection();
    assert(clay_config_set_reasoning_effort("max") == 0);
    ClayCommands *effort_commands = clay_commands_create(NULL);
    assert(strcmp(clay_commands_reasoning_effort(effort_commands)->id, "max") == 0);
    assert(strcmp(clay_commands_reasoning_effort(effort_commands)->label, "Max") == 0);
    clay_commands_destroy(effort_commands);
    assert(clay_sandbox_supported()); /* the 4-way cycle needs a real sandbox */

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    commands.sandbox_mode = CLAY_SANDBOX_MODE_SANDBOX;
    commands.sandbox_auto_approve = 0;

    /* Shift+Tab order: Sandbox (Ask) - Sandbox (Auto) - Unleashed (Ask) -
       Unleashed (Auto) - back to the start. */
    clay_commands_cycle_sandbox(&commands);
    assert_mode(&commands, CLAY_SANDBOX_MODE_SANDBOX, 1, "sandbox-auto");
    clay_commands_cycle_sandbox(&commands);
    assert_mode(&commands, CLAY_SANDBOX_MODE_UNLEASHED, 0, "unleashed");
    clay_commands_cycle_sandbox(&commands);
    assert_mode(&commands, CLAY_SANDBOX_MODE_UNLEASHED, 1, "unleashed-auto");
    clay_commands_cycle_sandbox(&commands);
    assert_mode(&commands, CLAY_SANDBOX_MODE_SANDBOX, 0, "sandbox");

    ClaySandboxMode mode;
    int auto_approve;
    clay_commands_parse_sandbox_mode("sandbox", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_SANDBOX && !auto_approve);
    clay_commands_parse_sandbox_mode("sandbox-auto", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_SANDBOX && auto_approve);
    clay_commands_parse_sandbox_mode("unleashed", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_UNLEASHED && !auto_approve);
    clay_commands_parse_sandbox_mode("unleashed-auto", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_UNLEASHED && auto_approve);
    /* Written by older versions. */
    clay_commands_parse_sandbox_mode("auto", &mode, &auto_approve);
    assert(mode == CLAY_SANDBOX_MODE_SANDBOX && auto_approve);

    printf("mode tests passed\n");
    return 0;
}
