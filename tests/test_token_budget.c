#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every chat pays for the whole system prompt plus every tool schema on
   every request, so this is the fixed cost of talking to a provider at all.
   The ceiling is a budget, not a measurement: raise it only with the reason
   in the commit message. killy spends 12,000 characters here (system prompt
   plus its 17 tools), and this is the number #89 has to bring clay toward. */
#define CLAY_PROMPT_AND_TOOL_BUDGET 22000

static size_t serialized_tool_bytes(const ClayToolSet *set) {
    size_t total = 0;
    for (size_t i = 0; i < set->tools.count; i++) {
        ClayTool *tool = clay_array_get((ClayArray *)&set->tools, i);
        total += strlen(tool->name);
        total += strlen(tool->description ? tool->description : "");
        ClayStr parameters;
        clay_str_init(&parameters);
        clay_json_stringify(tool->parameters, &parameters);
        total += parameters.len;
        clay_str_free(&parameters);
    }
    return total;
}

int main(void) {
    ClayApp *app = clay_app_create();
    ClayCommands *commands = clay_commands_create(app);
    assert(commands);

    for (int with_tools = 1; with_tools >= 0; with_tools--) {
        ClayToolSet set;
        clay_commands_tools_build(commands, &commands->plan, &set, with_tools);
        size_t tools = serialized_tool_bytes(&set);
        printf("%s: %zu tools, %zu bytes of schemas\n",
               with_tools ? "main agent" : "subagent", set.tools.count, tools);
        /* A subagent gets two fewer tools, so it can only be smaller. */
        if (!with_tools) assert(tools < CLAY_PROMPT_AND_TOOL_BUDGET);
        clay_commands_tools_free(&set);
    }

    ClayToolSet set;
    clay_commands_tools_build(commands, &commands->plan, &set, 1);
    size_t prompt = strlen(clay_commands_system_prompt_base());
    size_t tools = serialized_tool_bytes(&set);
    clay_commands_tools_free(&set);
    printf("system prompt: %zu bytes\n", prompt);
    printf("prompt + tools: %zu bytes (budget %d)\n", prompt + tools,
           CLAY_PROMPT_AND_TOOL_BUDGET);
    assert(prompt > 0 && tools > 0);
    assert(prompt + tools <= CLAY_PROMPT_AND_TOOL_BUDGET);

    /* The breakdown is what #89 acts on: a section that grows shows up on
       its own line instead of in the total. */
    ClayCommandsPromptSections sections;
    clay_commands_system_prompt_sections(&sections);
    printf("sections: base %zu, memory %zu, skills %zu, instructions %zu\n",
           sections.base, sections.memory, sections.skills,
           sections.instructions);
    assert(sections.base == prompt);
    assert(sections.total == sections.base + sections.memory +
                                 sections.skills + sections.instructions);
    /* With no memory, skills or AGENTS.md in this HOME, only the base is
       left - and a section that appears must land in the total. */
    assert(sections.total >= sections.base);

    clay_commands_destroy(commands);
    clay_app_destroy(app);
    return 0;
}
