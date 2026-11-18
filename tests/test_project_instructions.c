#include "../src/commands/context.h"

#include "clay/storage.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(content, file);
    fclose(file);
}

/* A private HOME, so the global instructions under ~/.clay are this test's
   own and the machine's are never read. The template stays at file scope
   because mkdtemp rewrites and returns its argument. */
static char home_template[] = "/tmp/clay_test_home_XXXXXX";

static char *make_home(void) {
    char *home = mkdtemp(home_template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);
    assert(clay_storage_ensure_dir("") == 0);
    return home;
}

static char *joined_path(const char *dir, const char *name) {
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/%s", dir, name);
    return path.data;
}

int main(void) {
    char template[] = "/tmp/clay_test_project_XXXXXX";
    char *root = mkdtemp(template);
    assert(root);

    char *home = make_home();
    char *global_agents = joined_path(home, ".clay/AGENTS.md");
    char *global_clay_md = joined_path(home, ".clay/CLAY.md");

    /* No global instructions: NULL, not an empty string. */
    assert(clay_commands_load_global_instructions() == NULL);

    /* CLAY.md stands in when there is no AGENTS.md, and AGENTS.md wins when
       both are present - the same rule the per-directory reader uses. */
    write_file(global_clay_md, "Global fallback.");
    char *global = clay_commands_load_global_instructions();
    assert(global && strcmp(global, "Global fallback.") == 0);
    free(global);
    write_file(global_agents, "Global preferences.");
    global = clay_commands_load_global_instructions();
    assert(global && strcmp(global, "Global preferences.") == 0);
    free(global);
    assert(remove(global_clay_md) == 0);

    /* Over the 8 KB cap it is cut and says where the rest is. */
    {
        ClayStr huge;
        clay_str_init(&huge);
        for (int i = 0; i < 10 * 1024; i++)
            clay_str_push_char(&huge, 'g');
        write_file(global_agents, huge.data);
        clay_str_free(&huge);
        global = clay_commands_load_global_instructions();
        assert(global);
        assert(strlen(global) < 10 * 1024);
        assert(strstr(global, "bytes of global instructions cut"));
        assert(strstr(global, "~/.clay/AGENTS.md"));
        free(global);
    }
    assert(remove(global_agents) == 0);

    /* No AGENTS.md/CLAY.md anywhere: NULL. */
    assert(chdir(root) == 0);
    char *instructions = clay_commands_load_project_instructions();
    assert(instructions == NULL);

    /* A repo root with AGENTS.md, and a subdirectory with its own CLAY.md,
       concatenated root-first. */
    char *repo = joined_path(root, "repo");
    char *git_dir = joined_path(repo, ".git");
    char *sub = joined_path(repo, "sub");
    assert(mkdir(repo, 0755) == 0);
    assert(mkdir(git_dir, 0755) == 0);
    assert(mkdir(sub, 0755) == 0);

    char *agents_path = joined_path(repo, "AGENTS.md");
    write_file(agents_path, "Root instructions.");
    char *clay_md_path = joined_path(sub, "CLAY.md");
    write_file(clay_md_path, "Sub instructions.");

    assert(chdir(sub) == 0);
    instructions = clay_commands_load_project_instructions();
    assert(instructions);
    /* Root-first: "Root instructions." appears before "Sub instructions.". */
    char *root_pos = strstr(instructions, "Root instructions.");
    char *sub_pos = strstr(instructions, "Sub instructions.");
    assert(root_pos && sub_pos && root_pos < sub_pos);
    free(instructions);

    /* In the prompt, the user's own instructions come before the project's:
       the more general text first, so a repo can override a preference. */
    write_file(global_agents, "Global preferences.");
    {
        ClayApp *app = clay_app_create();
        ClayCommands *commands = clay_commands_create(app);
        assert(commands);
        clay_commands_reset_conversation(commands);
        assert(commands->system_prompt);
        char *global_pos = strstr(commands->system_prompt, "Global preferences.");
        char *project_pos = strstr(commands->system_prompt, "Root instructions.");
        assert(strstr(commands->system_prompt, "Global user instructions"));
        assert(global_pos && project_pos && global_pos < project_pos);
        clay_commands_destroy(commands);
        clay_app_destroy(app);
    }
    assert(remove(global_agents) == 0);

    /* Above the repo root (outside .git), nothing above should be read
       even if it had its own AGENTS.md. */
    char *outside_agents = joined_path(root, "AGENTS.md");
    write_file(outside_agents, "Should never be seen.");
    instructions = clay_commands_load_project_instructions();
    assert(instructions);
    assert(!strstr(instructions, "Should never be seen."));
    free(instructions);

    /* Worktrees use a .git file rather than a directory. */
    assert(rmdir(git_dir) == 0);
    write_file(git_dir, "gitdir: /some/other/repo\n");
    instructions = clay_commands_load_project_instructions();
    assert(instructions);
    root_pos = strstr(instructions, "Root instructions.");
    sub_pos = strstr(instructions, "Sub instructions.");
    assert(root_pos && sub_pos && root_pos < sub_pos);
    assert(!strstr(instructions, "Should never be seen."));
    free(instructions);
    assert(remove(git_dir) == 0);

    /* A 40 KB root file is cut to the 32 KB limit, says so, and names where
       the rest is; the more specific file near the cwd is what gets lost. */
    {
        ClayStr huge;
        clay_str_init(&huge);
        for (int i = 0; i < 40 * 1024; i++)
            clay_str_push_char(&huge, 'r');
        write_file(agents_path, huge.data);
        clay_str_free(&huge);
        instructions = clay_commands_load_project_instructions();
        assert(instructions);
        assert(strlen(instructions) < 40 * 1024);
        assert(strstr(instructions, "bytes of project instructions cut"));
        assert(strstr(instructions, "AGENTS.md/CLAY.md"));
        assert(!strstr(instructions, "Sub instructions."));
        free(instructions);
    }

    remove(outside_agents);
    remove(clay_md_path);
    remove(agents_path);
    assert(rmdir(sub) == 0);
    assert(rmdir(repo) == 0);
    assert(chdir("/tmp") == 0);
    assert(rmdir(root) == 0);

    /* The HOME this test made, torn down so a run leaves nothing behind.
       `home` is the template array itself, so it is never freed. Building a
       prompt also caches it under ~/.clay. */
    free(global_clay_md);
    free(global_agents);
    char *cached = joined_path(home, ".clay/system_prompt.json");
    remove(cached);
    free(cached);
    char *clay_dir = joined_path(home, ".clay");
    assert(rmdir(clay_dir) == 0);
    free(clay_dir);
    assert(rmdir(home) == 0);

    free(outside_agents);
    free(clay_md_path);
    free(agents_path);
    free(sub);
    free(git_dir);
    free(repo);

    printf("project instructions tests passed\n");
    return 0;
}
