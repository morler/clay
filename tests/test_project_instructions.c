#include "../src/commands/context.h"

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

    free(outside_agents);
    free(clay_md_path);
    free(agents_path);
    free(sub);
    free(git_dir);
    free(repo);

    printf("project instructions tests passed\n");
    return 0;
}
