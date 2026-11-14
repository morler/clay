#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "context.h"

#include "clay/storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <dirent.h>
#if defined(__has_include)
#if __has_include(<linux/openat2.h>) && !defined(CLAY_NO_OPENAT2)
#include <linux/openat2.h>
#define CLAY_HAVE_OPENAT2 1
#endif
#endif
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define CLAY_FS_READ_LIMIT (256 * 1024)
#define CLAY_FS_GREP_LIMIT (64 * 1024)      /* shown inline to the model */
#define CLAY_FS_GREP_CAPTURE_LIMIT (4 * 1024 * 1024) /* captured, for the scratch dump */
#define CLAY_FS_GLOB_MAX_MATCHES 500
#define CLAY_FS_READ_SUGGESTION_LIMIT 5

/* Resolves `path` (relative to the workspace, or absolute) to a normalized
   absolute path in `abs_out`. Returns 0 if the result stays inside
   `workspace_dir`, -1 if it would escape it. */
int clay_fs_resolve_workspace_path(const char *workspace_dir, const char *path, ClayStr *abs_out) {
    ClayStr joined;
    clay_str_init(&joined);
    if (path[0] == '/') clay_str_push(&joined, path);
    else {
        clay_str_push(&joined, workspace_dir);
        clay_str_push_char(&joined, '/');
        clay_str_push(&joined, path);
    }

    char *dup = strdup(joined.data);
    clay_str_free(&joined);
    ClayArray stack;
    clay_array_init(&stack, sizeof(char *));
    char *p = dup;
    while (*p == '/') p++;
    char *token = p;
    for (;; p++) {
        if (*p == '/' || *p == '\0') {
            int end = *p == '\0';
            *p = '\0';
            if (*token && strcmp(token, ".") != 0) {
                if (strcmp(token, "..") == 0) {
                    if (stack.count > 0) clay_array_remove(&stack, stack.count - 1);
                } else clay_array_push_val(&stack, &token);
            }
            if (end) break;
            token = p + 1;
        }
    }
    clay_str_init(abs_out);
    clay_str_push_char(abs_out, '/');
    for (size_t i = 0; i < stack.count; i++) {
        if (i > 0) clay_str_push_char(abs_out, '/');
        clay_str_push(abs_out, *(char **)clay_array_get(&stack, i));
    }
    clay_array_free(&stack);
    free(dup);

    size_t wlen = strlen(workspace_dir);
    int contained = strncmp(abs_out->data, workspace_dir, wlen) == 0 &&
                    ((wlen == 1 && workspace_dir[0] == '/') ||
                     abs_out->data[wlen] == '/' || abs_out->data[wlen] == '\0');
    if (!contained) return -1;
    char *component = abs_out->data + wlen;
    if (wlen == 1) component = abs_out->data + 1;
    while (*component) {
        if (*component == '/') component++;
        char *end = strchr(component, '/');
        if (end) *end = '\0';
#ifdef _WIN32
        DWORD attrs = GetFileAttributesA(abs_out->data);
        int bad = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
        if (attrs == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND &&
            GetLastError() != ERROR_PATH_NOT_FOUND) bad = 1;
#else
        struct stat st;
        int rc = lstat(abs_out->data, &st);
        int bad = rc == 0 ? S_ISLNK(st.st_mode) : errno != ENOENT && errno != ENOTDIR;
#endif
        if (end) *end = '/';
        if (bad) return -1;
        if (!end) break;
        component = end;
    }
    return 0;
}

static int write_whole_file(const char *path, const char *data, size_t len) {
    return clay_term_write_project_file_atomic(path, data, len);
}

/* Creates every directory above `abs_path`'s final component. */
static int make_parent_dirs(const char *abs_path) {
    const char *slash = strrchr(abs_path, '/');
    if (!slash || slash == abs_path) return 1;
    ClayStr partial;
    clay_str_init(&partial);
    const char *p = abs_path;
    if (*p == '/') {
        clay_str_push_char(&partial, '/');
        p++;
    }
    int ok = 1;
    while (p < slash) {
        const char *next = strchr(p, '/');
        if (!next || next > slash) next = slash;
        clay_str_push_n(&partial, p, (size_t)(next - p));
        if (partial.len > 0 && clay_term_mkdir(partial.data) != 0) {
            ok = 0;
            break;
        }
        p = next + 1;
        if (p <= slash) clay_str_push_char(&partial, '/');
    }
    clay_str_free(&partial);
    return ok;
}

/* Common path arg handling shared by read/write/edit. On success returns 0
   and fills abs_out/workspace_out (caller frees both); on failure fills
   *result_out with the error tool result to return immediately. */
static int resolve_arg_path(const ClayJson *arguments, ClayStr *abs_out, char **workspace_out, ClayJson **result_out) {
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));
    if (!path || !*path) {
        *result_out = clay_json_object();
        clay_json_object_set(*result_out, "ok", clay_json_bool(0));
        clay_json_object_set(*result_out, "error", clay_json_string("path is required"));
        return -1;
    }
    *workspace_out = clay_term_cwd();
    if (clay_fs_resolve_workspace_path(*workspace_out, path, abs_out) != 0) {
        clay_str_free(abs_out);
        free(*workspace_out);
        *workspace_out = NULL;
        *result_out = clay_json_object();
        clay_json_object_set(*result_out, "ok", clay_json_bool(0));
        clay_json_object_set(*result_out, "error", clay_json_string("path escapes the workspace"));
        return -1;
    }
    return 0;
}

#ifdef __linux__
static int read_open_beneath(int root, const char *path, int flags) {
#if defined(CLAY_HAVE_OPENAT2) && defined(SYS_openat2)
    struct open_how how = { .flags = flags | O_CLOEXEC,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS };
    int fd = (int)syscall(SYS_openat2, root, *path ? path : ".", &how, sizeof(how));
    if (fd >= 0 || (errno != ENOSYS && errno != EINVAL && errno != E2BIG && errno != EPERM)) return fd;
#endif
    char *copy = strdup(*path ? path : ".");
    if (!copy) return -1;
    int dir = openat(root, ".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) { free(copy); return -1; }
    char *part = copy;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash) *slash = '\0';
        if (!*part || !strcmp(part, "..")) {
            close(dir); free(copy); errno = EINVAL; return -1;
        }
        int next = openat(dir, part, (slash ? O_PATH | O_DIRECTORY : flags) |
                          O_NOFOLLOW | O_CLOEXEC);
        int saved_errno = errno;
        close(dir);
        if (next < 0) { free(copy); errno = saved_errno; return -1; }
        if (!slash) { free(copy); return next; }
        dir = next;
        part = slash + 1;
    }
}

static void read_suggestions(int root, const char *prefix, const char *pattern,
                             ClayArray *matches, int *truncated, int *depth_truncated, unsigned depth) {
    /* shortcut: limit search depth to bound open fds, use an iterative walker if deeper suggestions are needed. */
    if (depth >= 16) { *depth_truncated = 1; return; }
    int fd = read_open_beneath(root, prefix, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return; }
    struct dirent *entry;
    while (!*truncated && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !strcmp(entry->d_name, ".git")) continue;
        ClayStr rel;
        clay_str_init(&rel);
        if (*prefix) { clay_str_push(&rel, prefix); clay_str_push_char(&rel, '/'); }
        clay_str_push(&rel, entry->d_name);
        int item = read_open_beneath(root, rel.data, O_PATH);
        struct stat st;
        if (item >= 0) {
            int stat_ok = fstat(item, &st) == 0;
            close(item);
            if (stat_ok && S_ISDIR(st.st_mode))
                read_suggestions(root, rel.data, pattern, matches, truncated, depth_truncated, depth + 1);
            else if (stat_ok && S_ISREG(st.st_mode) && clay_str_wildcard_match(pattern, rel.data)) {
                char *copy = strdup(rel.data);
                if (copy) clay_array_push_val(matches, &copy);
                if (matches->count >= CLAY_FS_GLOB_MAX_MATCHES) *truncated = 1;
            }
        }
        clay_str_free(&rel);
    }
    closedir(dir);
}
#endif

ClayJson *clay_fs_tool_read(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    ClayStr abs;
    char *workspace_dir = NULL;
    ClayJson *result = NULL;
    if (resolve_arg_path(arguments, &abs, &workspace_dir, &result) != 0) return result;

    long offset = (long)clay_json_number_value(clay_json_object_get(arguments, "offset"));
    long limit = (long)clay_json_number_value(clay_json_object_get(arguments, "limit"));
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));

    result = clay_json_object();
    clay_json_object_set(result, "path", clay_json_string(path));
#ifdef __linux__
    /* Anchor resolution to the working directory. */
    int root = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    int fd = -1;
    if (root >= 0) {
        size_t wlen = strlen(workspace_dir);
        const char *relative = abs.data + (wlen == 1 ? 1 : wlen);
        if (*relative == '/') relative++;
        fd = read_open_beneath(root, relative, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) != 0) {
                int saved_errno = errno;
                close(fd); fd = -1; errno = saved_errno;
            } else if (!S_ISREG(st.st_mode)) {
                close(fd); fd = -1; errno = EINVAL;
            }
        }
        int saved_errno = errno;
        close(root);
        errno = saved_errno;
    }
    FILE *file = fd < 0 ? NULL : fdopen(fd, "rb");
    if (fd >= 0 && !file) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
#else
    FILE *file = fopen(abs.data, "rb");
#endif
    int open_errno = errno;
    free(workspace_dir);
    clay_str_free(&abs);
    errno = open_errno;
    if (!file) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        ClayStr error;
        clay_str_init(&error);
        clay_str_push(&error, strerror(errno));
        if (errno == ENOENT) {
            const char *base_name = path;
            for (const char *p = path; *p; p++) {
                if (*p == '/') base_name = p + 1;
            }
            if (*base_name) {
                char *ws = clay_term_cwd();
                ClayStr suggest_pattern;
                clay_str_init(&suggest_pattern);
                clay_str_printf(&suggest_pattern, "*%s*", base_name);
                ClayArray matches;
                clay_array_init(&matches, sizeof(char *));
                int suggest_truncated = 0;
#ifdef __linux__
                int depth_truncated = 0;
                int suggest_root = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
                if (suggest_root >= 0) {
                    read_suggestions(suggest_root, "", suggest_pattern.data, &matches, &suggest_truncated, &depth_truncated, 0);
                    close(suggest_root);
                }
                if (depth_truncated) clay_str_push(&error, " (suggestion search depth limit reached)");
#else
                clay_fs_walk_files(ws, "", suggest_pattern.data, &matches, &suggest_truncated);
#endif
                clay_str_free(&suggest_pattern);
                free(ws);
                if (matches.count > 0) {
                    clay_str_push(&error, " - did you mean: ");
                    size_t shown = matches.count < CLAY_FS_READ_SUGGESTION_LIMIT ? matches.count : CLAY_FS_READ_SUGGESTION_LIMIT;
                    for (size_t i = 0; i < shown; i++) {
                        if (i > 0) clay_str_push(&error, ", ");
                        clay_str_push(&error, *(char **)clay_array_get(&matches, i));
                    }
                    clay_str_push_char(&error, '?');
                }
                for (size_t i = 0; i < matches.count; i++) free(*(char **)clay_array_get(&matches, i));
                clay_array_free(&matches);
            }
        }
        clay_json_object_set(result, "error", clay_json_string(error.data));
        clay_str_free(&error);
        return result;
    }

    ClayStr formatted;
    clay_str_init(&formatted);
    ClayStr line;
    clay_str_init(&line);
    size_t line_no = 0;
    size_t emitted = 0;
    int truncated = 0;
    int at_line_start = 1;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (at_line_start) {
            line_no++;
            at_line_start = 0;
        }
        if (ch == '\n') {
            if (line_no > (size_t)(offset > 0 ? offset - 1 : 0) && (limit <= 0 || emitted < (size_t)limit)) {
                clay_str_printf(&formatted, "%5zu\t%s\n", line_no, line.data);
                emitted++;
            }
            clay_str_clear(&line);
            at_line_start = 1;
            if (formatted.len > CLAY_FS_READ_LIMIT) {
                truncated = 1;
                break;
            }
            continue;
        }
        clay_str_push_char(&line, (char)ch);
    }
    if (!truncated && !at_line_start && line.len > 0 &&
        line_no > (size_t)(offset > 0 ? offset - 1 : 0) && (limit <= 0 || emitted < (size_t)limit)) {
        clay_str_printf(&formatted, "%5zu\t%s\n", line_no, line.data);
    }
    clay_str_free(&line);
    fclose(file);

    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(formatted.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(truncated));
    clay_json_object_set(result, "total_lines", clay_json_number((double)line_no));
    clay_str_free(&formatted);
    return result;
}

ClayJson *clay_fs_tool_read_schema(void) {
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description", clay_json_string("File path, relative to the workspace root or absolute."));
    ClayJson *offset = clay_json_object();
    clay_json_object_set(offset, "type", clay_json_string("number"));
    clay_json_object_set(offset, "description", clay_json_string("1-based line number to start from. Omit to start at line 1."));
    ClayJson *limit = clay_json_object();
    clay_json_object_set(limit, "type", clay_json_string("number"));
    clay_json_object_set(limit, "description", clay_json_string("Max number of lines to return. Omit to read the whole file."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "path", path);
    clay_json_object_set(properties, "offset", offset);
    clay_json_object_set(properties, "limit", limit);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("path"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

ClayJson *clay_fs_tool_write(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    ClayStr abs;
    char *workspace_dir = NULL;
    ClayJson *result = NULL;
    if (resolve_arg_path(arguments, &abs, &workspace_dir, &result) != 0) return result;
    free(workspace_dir);

    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));
    const char *content = clay_json_string_value(clay_json_object_get(arguments, "content"));
    if (!content) content = "";

    result = clay_json_object();
    clay_json_object_set(result, "path", clay_json_string(path));
    if (!make_parent_dirs(abs.data) || write_whole_file(abs.data, content, strlen(content)) != 0) {
        clay_str_free(&abs);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("could not write file"));
        return result;
    }
    clay_str_free(&abs);

    size_t lines = 1;
    for (const char *p = content; *p; p++) {
        if (*p == '\n') lines++;
    }
    ClayStr summary;
    clay_str_init(&summary);
    clay_str_printf(&summary, "Wrote %zu bytes (%zu lines).", strlen(content), lines);
    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(summary.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    clay_str_free(&summary);
    return result;
}

ClayJson *clay_fs_tool_write_schema(void) {
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description",
                         clay_json_string("File path, relative to the workspace root or absolute. Overwrites if it "
                                          "already exists; parent directories are created as needed."));
    ClayJson *content = clay_json_object();
    clay_json_object_set(content, "type", clay_json_string("string"));
    clay_json_object_set(content, "description", clay_json_string("Full file content to write."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "path", path);
    clay_json_object_set(properties, "content", content);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("path"));
    clay_json_array_push(required, clay_json_string("content"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

static size_t count_occurrences(const char *haystack, const char *needle) {
    size_t count = 0;
    size_t needle_len = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

/* Replaces the first occurrence of `needle` (or every occurrence, if
   replace_all) with `repl`, appending the result to `out`. */
static void apply_replacement(ClayStr *out, const char *haystack, const char *needle, const char *repl,
                              int replace_all) {
    size_t needle_len = strlen(needle);
    const char *p = haystack;
    int replaced_one = 0;
    for (;;) {
        const char *match = strstr(p, needle);
        if (!match || (!replace_all && replaced_one)) {
            clay_str_push(out, p);
            return;
        }
        clay_str_push_n(out, p, (size_t)(match - p));
        clay_str_push(out, repl);
        replaced_one = 1;
        p = match + needle_len;
    }
}

static void push_prefixed_lines(ClayStr *out, const char *text, const char *prefix) {
    const char *p = text;
    while (*p) {
        const char *newline = strchr(p, '\n');
        size_t len = newline ? (size_t)(newline - p) : strlen(p);
        clay_str_push(out, prefix);
        clay_str_push_n(out, p, len);
        clay_str_push_char(out, '\n');
        if (!newline) break;
        p = newline + 1;
    }
}

ClayJson *clay_fs_tool_edit(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    const char *old_string = clay_json_string_value(clay_json_object_get(arguments, "oldString"));
    const char *new_string = clay_json_string_value(clay_json_object_get(arguments, "newString"));
    int replace_all = clay_json_bool_value(clay_json_object_get(arguments, "replaceAll"));
    if (!new_string) new_string = "";
    if (!old_string || !*old_string) {
        ClayJson *result = clay_json_object();
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("oldString is required and must be non-empty"));
        return result;
    }

    ClayStr abs;
    char *workspace_dir = NULL;
    ClayJson *result = NULL;
    if (resolve_arg_path(arguments, &abs, &workspace_dir, &result) != 0) return result;
    free(workspace_dir);
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));

    result = clay_json_object();
    clay_json_object_set(result, "path", clay_json_string(path));
    ClayStr content;
    if (clay_storage_read_limited(abs.data, CLAY_FS_READ_LIMIT, &content) != 0) {
        clay_str_free(&abs);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string(strerror(errno)));
        return result;
    }

    size_t occurrences = count_occurrences(content.data, old_string);
    if (occurrences == 0) {
        clay_str_free(&abs);
        clay_str_free(&content);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("oldString was not found in the file"));
        return result;
    }
    if (occurrences > 1 && !replace_all) {
        clay_str_free(&abs);
        clay_str_free(&content);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(
            result, "error",
            clay_json_string("oldString is not unique in the file - include more context, or pass replaceAll"));
        return result;
    }

    ClayStr updated;
    clay_str_init(&updated);
    apply_replacement(&updated, content.data, old_string, new_string, replace_all);
    clay_str_free(&content);

    if (write_whole_file(abs.data, updated.data, updated.len) != 0) {
        clay_str_free(&abs);
        clay_str_free(&updated);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("could not write file"));
        return result;
    }
    clay_str_free(&abs);
    clay_str_free(&updated);

    ClayStr diff;
    clay_str_init(&diff);
    push_prefixed_lines(&diff, old_string, "- ");
    push_prefixed_lines(&diff, new_string, "+ ");
    if (replace_all && occurrences > 1) clay_str_printf(&diff, "(%zu replacements)", occurrences);
    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(diff.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    clay_json_object_set(result, "replacements", clay_json_number((double)(replace_all ? occurrences : 1)));
    clay_str_free(&diff);
    return result;
}

ClayJson *clay_fs_tool_edit_schema(void) {
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description", clay_json_string("File path, relative to the workspace root or absolute."));
    ClayJson *old_string = clay_json_object();
    clay_json_object_set(old_string, "type", clay_json_string("string"));
    clay_json_object_set(
        old_string, "description",
        clay_json_string("Exact text to replace. Must match byte-for-byte, including whitespace. Include enough "
                         "surrounding context to make it unique in the file unless replaceAll is set."));
    ClayJson *new_string = clay_json_object();
    clay_json_object_set(new_string, "type", clay_json_string("string"));
    clay_json_object_set(new_string, "description", clay_json_string("Replacement text."));
    ClayJson *replace_all = clay_json_object();
    clay_json_object_set(replace_all, "type", clay_json_string("boolean"));
    clay_json_object_set(replace_all, "description",
                         clay_json_string("Replace every occurrence instead of requiring oldString to be unique."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "path", path);
    clay_json_object_set(properties, "oldString", old_string);
    clay_json_object_set(properties, "newString", new_string);
    clay_json_object_set(properties, "replaceAll", replace_all);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("path"));
    clay_json_array_push(required, clay_json_string("oldString"));
    clay_json_array_push(required, clay_json_string("newString"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

void clay_fs_walk_files(const char *base_dir, const char *rel_prefix, const char *pattern, ClayArray *matches,
                      int *truncated) {
    if (*truncated || matches->count >= CLAY_FS_GLOB_MAX_MATCHES) {
        *truncated = 1;
        return;
    }
    ClayArray entries;
    if (clay_term_list_entries(base_dir, &entries) != 0) return;
    for (size_t i = 0; i < entries.count; i++) {
        char *name = *(char **)clay_array_get(&entries, i);
        if (strcmp(name, ".git") == 0) continue;

        ClayStr rel;
        clay_str_init(&rel);
        if (*rel_prefix) {
            clay_str_push(&rel, rel_prefix);
            clay_str_push_char(&rel, '/');
        }
        clay_str_push(&rel, name);

        ClayStr full;
        clay_str_init(&full);
        clay_str_push(&full, base_dir);
        if (full.len && full.data[full.len - 1] != '/') clay_str_push_char(&full, '/');
        clay_str_push(&full, name);
#ifdef _WIN32
        DWORD attrs = GetFileAttributesA(full.data);
        int direct = attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
        struct stat st;
        int direct = lstat(full.data, &st) == 0 && !S_ISLNK(st.st_mode);
#endif

        if (direct && clay_term_is_dir(full.data)) {
            clay_fs_walk_files(full.data, rel.data, pattern, matches, truncated);
        } else if (direct && clay_str_wildcard_match(pattern, rel.data)) {
            if (matches->count >= CLAY_FS_GLOB_MAX_MATCHES) *truncated = 1;
            else {
                char *copy = strdup(rel.data);
                clay_array_push_val(matches, &copy);
            }
        }
        clay_str_free(&rel);
        clay_str_free(&full);
    }
    for (size_t i = 0; i < entries.count; i++) free(*(char **)clay_array_get(&entries, i));
    clay_array_free(&entries);
}

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

ClayJson *clay_fs_tool_glob(const ClayJson *arguments, void *userdata) {
    (void)userdata;
    const char *pattern = clay_json_string_value(clay_json_object_get(arguments, "pattern"));
    const char *base = clay_json_string_value(clay_json_object_get(arguments, "path"));
    if (!base || !*base) base = ".";
    ClayJson *result = clay_json_object();
    if (!pattern || !*pattern) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("pattern is required"));
        return result;
    }
    clay_json_object_set(result, "pattern", clay_json_string(pattern));

    char *workspace_dir = clay_term_cwd();
    ClayStr abs_base;
    if (clay_fs_resolve_workspace_path(workspace_dir, base, &abs_base) != 0) {
        free(workspace_dir);
        clay_str_free(&abs_base);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("path escapes the workspace"));
        return result;
    }
    free(workspace_dir);

    ClayArray matches;
    clay_array_init(&matches, sizeof(char *));
    int truncated = 0;
    clay_fs_walk_files(abs_base.data, "", pattern, &matches, &truncated);
    clay_str_free(&abs_base);

    if (matches.count > 0) qsort(matches.data, matches.count, sizeof(char *), compare_strings);
    ClayStr output;
    clay_str_init(&output);
    for (size_t i = 0; i < matches.count; i++) {
        clay_str_push(&output, *(char **)clay_array_get(&matches, i));
        clay_str_push_char(&output, '\n');
        free(*(char **)clay_array_get(&matches, i));
    }
    clay_array_free(&matches);

    clay_json_object_set(result, "ok", clay_json_bool(1));
    clay_json_object_set(result, "output", clay_json_string(output.data));
    clay_json_object_set(result, "output_truncated", clay_json_bool(truncated));
    clay_str_free(&output);
    return result;
}

ClayJson *clay_fs_tool_glob_schema(void) {
    ClayJson *pattern = clay_json_object();
    clay_json_object_set(pattern, "type", clay_json_string("string"));
    clay_json_object_set(
        pattern, "description",
        clay_json_string("Wildcard pattern matched against each file's path relative to `path` (e.g. \"*.c\", "
                         "\"src/*.c\", \"*/test_*.c\"). '*' matches any run of characters including '/'; '?' "
                         "matches one character."));
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description", clay_json_string("Directory to search from. Defaults to the workspace root."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "pattern", pattern);
    clay_json_object_set(properties, "path", path);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("pattern"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}

ClayJson *clay_fs_tool_grep(const ClayJson *arguments, void *userdata) {
    ClayCommands *commands = userdata;
    const char *pattern = clay_json_string_value(clay_json_object_get(arguments, "pattern"));
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));
    const char *include = clay_json_string_value(clay_json_object_get(arguments, "glob"));
    if (!path || !*path) path = ".";
    ClayJson *result = clay_json_object();
    if (!pattern || !*pattern) {
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("pattern is required"));
        return result;
    }
    clay_json_object_set(result, "pattern", clay_json_string(pattern));

    char *workspace_dir = clay_term_cwd();
    ClayStr target;
    if (!workspace_dir || clay_fs_resolve_workspace_path(workspace_dir, path, &target) != 0) {
        if (workspace_dir) clay_str_free(&target);
        free(workspace_dir);
        clay_json_object_set(result, "ok", clay_json_bool(0));
        clay_json_object_set(result, "error", clay_json_string("path escapes the workspace or contains a symlink"));
        return result;
    }
    ClayStr invocation;
    clay_str_init(&invocation);
    clay_str_push(&invocation, "grep -Ern -I --binary-files=without-match -e ");
    clay_term_shell_quote(&invocation, pattern);
    if (include && *include) {
        clay_str_push(&invocation, " --include=");
        clay_term_shell_quote(&invocation, include);
    }
    clay_str_push(&invocation, " -- ");
    const char *relative = target.data + strlen(workspace_dir);
    while (*relative == '/') relative++;
    clay_term_shell_quote(&invocation, *relative ? relative : ".");
    clay_str_free(&target);
    char *scratch_dir = clay_chat_scratch_dir(commands->chat);
    ClaySandboxConfig sandbox = {
        .mode = commands->sandbox_mode,
        .shared = commands->sandbox_namespaces,
        .workspace_dir = workspace_dir,
        .scratch_dir = scratch_dir,
    };
    ClayStr output;
    clay_str_init(&output);
    ClayExecResult exec = {0};
    int rc = clay_sandbox_exec(&sandbox, invocation.data, &output, CLAY_FS_GREP_CAPTURE_LIMIT, NULL, &exec);
    int exit_code = exec.exit_code;
    int truncated = exec.output_truncated;
    free(workspace_dir);
    free(scratch_dir);
    clay_str_free(&invocation);

    /* grep exits 1 for "no matches", which is a normal empty result, and
       >1 for a real error (bad pattern, missing path, ...). */
    int ok = rc == 0 && exit_code <= 1;
    clay_json_object_set(result, "ok", clay_json_bool(ok));
    if (exit_code == 1) {
        clay_json_object_set(result, "output", clay_json_string(""));
        clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    } else if (output.len > CLAY_FS_GREP_LIMIT) {
        char *scratch_path = clay_chat_dump_scratch(commands->chat, "grep", output.data);
        ClayStr preview;
        clay_str_init(&preview);
        clay_str_push_n(&preview, output.data, CLAY_FS_GREP_LIMIT);
        if (scratch_path) clay_str_printf(&preview, "\n... (%zu bytes total, full output at %s)", output.len, scratch_path);
        else clay_str_push(&preview, "\n... (truncated)");
        clay_json_object_set(result, "output", clay_json_string(preview.data));
        clay_json_object_set(result, "output_truncated", clay_json_bool(1));
        if (scratch_path) clay_json_object_set(result, "scratch_path", clay_json_string(scratch_path));
        clay_str_free(&preview);
        free(scratch_path);
    } else {
        clay_json_object_set(result, "output", clay_json_string(output.data));
        clay_json_object_set(result, "output_truncated", clay_json_bool(truncated));
    }
    if (!ok) clay_json_object_set(result, "error", clay_json_string(rc != 0 ? "failed to start grep" : "grep failed"));
    clay_str_free(&output);
    return result;
}

ClayJson *clay_fs_tool_grep_schema(void) {
    ClayJson *pattern = clay_json_object();
    clay_json_object_set(pattern, "type", clay_json_string("string"));
    clay_json_object_set(pattern, "description", clay_json_string("Regular expression to search for (grep -E syntax)."));
    ClayJson *path = clay_json_object();
    clay_json_object_set(path, "type", clay_json_string("string"));
    clay_json_object_set(path, "description", clay_json_string("File or directory to search. Defaults to the workspace root."));
    ClayJson *glob = clay_json_object();
    clay_json_object_set(glob, "type", clay_json_string("string"));
    clay_json_object_set(glob, "description", clay_json_string("Only search files matching this glob, e.g. \"*.c\"."));
    ClayJson *properties = clay_json_object();
    clay_json_object_set(properties, "pattern", pattern);
    clay_json_object_set(properties, "path", path);
    clay_json_object_set(properties, "glob", glob);
    ClayJson *required = clay_json_array();
    clay_json_array_push(required, clay_json_string("pattern"));
    ClayJson *schema = clay_json_object();
    clay_json_object_set(schema, "type", clay_json_string("object"));
    clay_json_object_set(schema, "properties", properties);
    clay_json_object_set(schema, "required", required);
    clay_json_object_set(schema, "additionalProperties", clay_json_bool(0));
    return schema;
}
