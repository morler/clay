#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/memory.h"
#include "clay/skill.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A subagent pays for the main prompt, its own suffix and its tools on every
   run, and that sum was the one cost nothing measured. */
extern const char *clay_subagent_prompt_suffix(void);

/* Every chat pays for the whole system prompt plus every tool schema on
   every request, so this is the fixed cost of talking to a provider at all.
   The ceiling is a budget, not a measurement: raise it only with the reason
   in the commit message. killy spends 12,000 characters here (system prompt
   plus its 17 tools), and this is the number #89 has to bring clay toward. */
#define CLAY_PROMPT_AND_TOOL_BUDGET 17400

/* A subagent's own ceiling, measured at 16406 B. Kept apart from the main
   budget, so a change that only hurts subagents still fails. */
#define CLAY_SUBAGENT_FIXED_COST_BUDGET 16800

/* The base prompt plus a maximal store: memory, skills, global and project
   instructions each at their own ceiling still add up to a fixed amount,
   because the whole system prompt is the fixed prefix of every request.
   Each section adds a header and a note naming where the rest is kept;
   CLAY_SECTION_FRAMING_CEILING covers both. The total is also bounded on
   its own, so raising one section's cap still has to fit here. */
#define CLAY_SECTION_FRAMING_CEILING 512
#define CLAY_MEMORY_SECTION_CEILING \
    (CLAY_MEMORY_INDEX_MAX_BYTES + CLAY_SECTION_FRAMING_CEILING)
#define CLAY_SKILLS_SECTION_CEILING \
    (CLAY_SKILL_INDEX_MAX_BYTES + CLAY_SECTION_FRAMING_CEILING)
#define CLAY_INSTRUCTIONS_SECTION_CEILING \
    (CLAY_PROJECT_INSTRUCTIONS_MAX_BYTES + CLAY_SECTION_FRAMING_CEILING)
#define CLAY_GLOBAL_SECTION_CEILING \
    (CLAY_GLOBAL_INSTRUCTIONS_MAX_BYTES + CLAY_SECTION_FRAMING_CEILING)
#define CLAY_SYSTEM_PROMPT_BUDGET (60 * 1024)

/* Each tool's own line: with the descriptions and schemas at 8.5 KB, a
   regression has to name the tool that caused it, not move the total. */
static size_t serialized_tool_bytes(const ClayToolSet *set, int report) {
    size_t total = 0;
    for (size_t i = 0; i < set->tools.count; i++) {
        ClayTool *tool = clay_array_get((ClayArray *)&set->tools, i);
        size_t description = strlen(tool->description ? tool->description : "");
        ClayStr parameters;
        clay_str_init(&parameters);
        clay_json_stringify(tool->parameters, &parameters);
        size_t bytes = strlen(tool->name) + description + parameters.len;
        if (report)
            printf("  %-14s %5zu B (name %zu, desc %zu, params %zu)\n",
                   tool->name, bytes, strlen(tool->name), description,
                   parameters.len);
        total += bytes;
        clay_str_free(&parameters);
    }
    return total;
}

static void write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(content, file);
    fclose(file);
}

static char *joined_path(const char *dir, const char *name) {
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/%s", dir, name);
    return path.data;
}

/* A private HOME with every store at its ceiling, and a cwd inside a repo
   whose AGENTS.md is larger than the project-instruction cap. Everything
   below then measures a hostile store rather than whatever the machine that
   runs the test happens to have. mkdtemp rewrites and returns its template
   argument, so these stay alive at file scope. */
static char home_template[] = "/tmp/clay_budget_home_XXXXXX";
static char skill_source_template[] = "/tmp/clay_budget_skill_XXXXXX";
static char repo_template[] = "/tmp/clay_budget_repo_XXXXXX";
static char *home;
static char *skill_source;
static char *repo;

static void fill_maximal_store(void) {
    home = mkdtemp(home_template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    /* The user's own instructions live under ~/.clay too, so an overlong one
       has to fit in the same budget. */
    assert(clay_storage_ensure_dir("") == 0);
    char *global_path = joined_path(home, ".clay/AGENTS.md");
    ClayStr global_text;
    clay_str_init(&global_text);
    for (int i = 0; i < 10 * 1024; i++)
        clay_str_push_char(&global_text, 'g');
    write_file(global_path, global_text.data);
    clay_str_free(&global_text);
    free(global_path);

    ClayStr summary;
    clay_str_init(&summary);
    for (int i = 0; i < 300; i++)
        clay_str_push_char(&summary, 'm');
    for (int i = 0; i < 40; i++) {
        char slug[16];
        snprintf(slug, sizeof(slug), "entry-%02d", i);
        assert(clay_memory_write(slug, "task", summary.data, "body") == 0);
    }
    clay_str_free(&summary);

    skill_source = mkdtemp(skill_source_template);
    assert(skill_source);
    char *skill_md = joined_path(skill_source, "SKILL.md");
    ClayStr frontmatter;
    clay_str_init(&frontmatter);
    clay_str_push(&frontmatter, "---\nname: sample\ndescription: A skill description long enough to spend the byte "
                               "budget.\n---\nBody.\n");
    write_file(skill_md, frontmatter.data);
    clay_str_free(&frontmatter);
    for (int i = 0; i < 70; i++) {
        char name[16];
        snprintf(name, sizeof(name), "skill-%02d", i);
        assert(clay_skill_install(skill_source, name) == 0);
    }
    free(skill_md);

    repo = mkdtemp(repo_template);
    assert(repo);
    char *git_dir = joined_path(repo, ".git");
    assert(mkdir(git_dir, 0755) == 0);
    free(git_dir);
    char *agents = joined_path(repo, "AGENTS.md");
    ClayStr huge;
    clay_str_init(&huge);
    for (int i = 0; i < 40 * 1024; i++)
        clay_str_push_char(&huge, 'r');
    write_file(agents, huge.data);
    clay_str_free(&huge);
    free(agents);
    assert(chdir(repo) == 0);
}

/* Every path here came from mkdtemp, so no quoting is needed. */
static void remove_maximal_store(void) {
    assert(chdir("/tmp") == 0);
    ClayStr command;
    clay_str_init(&command);
    clay_str_printf(&command, "rm -rf %s %s %s", home, skill_source, repo);
    assert(system(command.data) == 0);
    clay_str_free(&command);
}

int main(void) {
    fill_maximal_store();

    ClayApp *app = clay_app_create();
    ClayCommands *commands = clay_commands_create(app);
    assert(commands);

    for (int with_tools = 1; with_tools >= 0; with_tools--) {
        ClayToolSet set;
        clay_commands_tools_build(commands, &commands->plan, &set, with_tools);
        size_t tools = serialized_tool_bytes(&set, 0);
        printf("%s: %zu tools, %zu bytes of schemas\n",
               with_tools ? "main agent" : "subagent", set.tools.count, tools);
        /* A subagent gets two fewer tools, so it can only be smaller. */
        if (!with_tools) assert(tools < CLAY_PROMPT_AND_TOOL_BUDGET);
        clay_commands_tools_free(&set);
    }

    ClayToolSet set;
    clay_commands_tools_build(commands, &commands->plan, &set, 1);
    size_t prompt = strlen(clay_commands_system_prompt_base());
    printf("per tool, largest first:\n");
    size_t tools = serialized_tool_bytes(&set, 1);
    clay_commands_tools_free(&set);
    printf("system prompt: %zu bytes\n", prompt);
    printf("prompt + tools: %zu bytes (budget %d)\n", prompt + tools,
           CLAY_PROMPT_AND_TOOL_BUDGET);
    assert(prompt > 0 && tools > 0);
    assert(prompt + tools <= CLAY_PROMPT_AND_TOOL_BUDGET);

    /* A subagent sends the same prompt and its own smaller tool set plus a
       suffix, so its fixed cost gets its own ceiling: a change that only
       hurts subagents must still fail. */
    {
        ClayToolSet subagent_tools;
        clay_commands_tools_build(commands, &commands->plan, &subagent_tools, 0);
        size_t subagent_bytes = serialized_tool_bytes(&subagent_tools, 0);
        clay_commands_tools_free(&subagent_tools);
        const char *suffix = clay_subagent_prompt_suffix();
        size_t suffix_bytes = strlen(suffix);
        printf("subagent fixed cost: prompt %zu + suffix %zu + tools %zu = "
               "%zu B (budget %d)\n",
               prompt, suffix_bytes, subagent_bytes,
               prompt + suffix_bytes + subagent_bytes,
               CLAY_SUBAGENT_FIXED_COST_BUDGET);
        assert(suffix_bytes > 0);
        assert(prompt + suffix_bytes + subagent_bytes <=
               CLAY_SUBAGENT_FIXED_COST_BUDGET);
    }

    /* A section that grows shows up on its own line instead of in the
       total. */
    ClayCommandsPromptSections sections;
    clay_commands_system_prompt_sections(&sections);
    printf("sections: base %zu, memory %zu, skills %zu, global %zu, "
           "instructions %zu\n",
           sections.base, sections.memory, sections.skills, sections.global,
           sections.instructions);
    assert(sections.base == prompt);
    assert(sections.total == sections.base + sections.memory +
                                 sections.skills + sections.global +
                                 sections.instructions);

    /* Each store was filled to its ceiling, so every section is there, sits
       under its own cap, and the sum is still bounded. */
    assert(sections.memory > 0 && sections.skills > 0 && sections.global > 0 &&
           sections.instructions > 0);
    printf("sections under a maximal store: memory %zu/%zu, skills %zu/%zu, "
           "global %zu/%zu, instructions %zu/%zu\n",
           sections.memory, (size_t)CLAY_MEMORY_SECTION_CEILING,
           sections.skills, (size_t)CLAY_SKILLS_SECTION_CEILING,
           sections.global, (size_t)CLAY_GLOBAL_SECTION_CEILING,
           sections.instructions, (size_t)CLAY_INSTRUCTIONS_SECTION_CEILING);
    assert(sections.memory <= CLAY_MEMORY_SECTION_CEILING);
    assert(sections.skills <= CLAY_SKILLS_SECTION_CEILING);
    assert(sections.global <= CLAY_GLOBAL_SECTION_CEILING);
    assert(sections.instructions <= CLAY_INSTRUCTIONS_SECTION_CEILING);
    size_t ceiling = sections.base + CLAY_MEMORY_SECTION_CEILING +
                     CLAY_SKILLS_SECTION_CEILING +
                     CLAY_GLOBAL_SECTION_CEILING +
                     CLAY_INSTRUCTIONS_SECTION_CEILING;
    printf("system prompt with a maximal store: %zu bytes (sections add up "
           "to %zu, budget %d)\n",
           sections.total, ceiling, CLAY_SYSTEM_PROMPT_BUDGET);
    assert(sections.total <= ceiling);
    assert(sections.total <= CLAY_SYSTEM_PROMPT_BUDGET);

    /* The breakdown describes the prompt that is actually sent, and every
       cut section says so and names where the rest is kept. */
    clay_commands_reset_conversation(commands);
    assert(commands->system_prompt);
    assert(strlen(commands->system_prompt) == sections.total);
    assert(strstr(commands->system_prompt, "memory_list and memory_read"));
    assert(strstr(commands->system_prompt, "/skill lists the rest"));
    assert(strstr(commands->system_prompt, "bytes of global instructions cut"));
    assert(strstr(commands->system_prompt, "bytes of project instructions cut"));

    assert(chdir("/tmp") == 0);

    clay_commands_destroy(commands);
    clay_app_destroy(app);
    remove_maximal_store();
    return 0;
}
