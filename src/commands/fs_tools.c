#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "context.h"

#include "clay/storage.h"
#ifdef __linux__
#include "clay/workspace_fd.h"
#endif

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
#include <fnmatch.h>
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
/* A whole file is rarely what the model needs, and one huge line (a minified
   bundle, a base64 blob) is never useful: page it and cut the line. */
#define CLAY_FS_READ_DEFAULT_LINES 2000
#define CLAY_FS_READ_LINE_LIMIT 2000
#define CLAY_FS_GREP_LIMIT (64 * 1024)      /* shown inline to the model */
#define CLAY_FS_GREP_CAPTURE_LIMIT (4 * 1024 * 1024) /* captured, for the scratch dump */
#define CLAY_FS_GREP_MAX_FILES 4096
#define CLAY_FS_GLOB_MAX_MATCHES 500
#define CLAY_FS_READ_SUGGESTION_LIMIT 5

static int win_sep(char c) { return c == '/' || c == '\\'; }
static int win_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static char win_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c; }

static int win_reserved(const char *s, size_t n) {
    size_t stem = 0;
    while (stem < n && s[stem] != '.') stem++;
    while (stem && s[stem - 1] == ' ') stem--;
    char name[8];
    if (stem >= sizeof(name)) return 0;
    for (size_t i = 0; i < stem; i++) name[i] = win_lower(s[i]);
    name[stem] = 0;
    if (!strcmp(name, "con") || !strcmp(name, "prn") || !strcmp(name, "aux") ||
        !strcmp(name, "nul") || !strcmp(name, "clock$") || !strcmp(name, "conin$") || !strcmp(name, "conout$")) return 1;
    if (stem >= 4 && (!strncmp(name, "com", 3) || !strncmp(name, "lpt", 3))) {
        if (stem == 4 && name[3] >= '1' && name[3] <= '9') return 1;
        if (stem == 5 && (unsigned char)name[3] == 0xc2 &&
            ((unsigned char)name[4] == 0xb9 || (unsigned char)name[4] == 0xb2 ||
             (unsigned char)name[4] == 0xb3)) return 1;
    }
    return 0;
}

static int win_normalize(const char *path, ClayStr *out) {
    clay_str_init(out);
    const char *p = path;
    if (win_alpha(p[0]) && p[1] == ':' && win_sep(p[2])) {
        clay_str_push_n(out, p, 2); clay_str_push_char(out, '/'); p += 3;
    } else if (win_sep(p[0]) && win_sep(p[1]) && p[2] && p[2] != '?' && p[2] != '.') {
        clay_str_push(out, "//"); p += 2;
        for (int i = 0; i < 2; i++) {
            const char *start = p;
            while (*p && !win_sep(*p)) {
                if ((unsigned char)*p < 32 || strchr(":*?\"<>|", *p)) return -1;
                p++;
            }
            if (p == start || p[-1] == '.' || p[-1] == ' ' || win_reserved(start, (size_t)(p - start))) return -1;
            if (i) clay_str_push_char(out, '/');
            clay_str_push_n(out, start, (size_t)(p - start));
            if (!i) { if (!*p) return -1; p++; }
        }
    } else return -1;
    size_t root = out->len;
    while (*p) {
        while (win_sep(*p)) p++;
        const char *start = p;
        while (*p && !win_sep(*p)) {
            if ((unsigned char)*p < 32 || strchr(":*?\"<>|", *p)) return -1;
            p++;
        }
        size_t n = (size_t)(p - start);
        if (!n || (n == 1 && *start == '.')) continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (out->len > root) {
                while (out->len > root && out->data[out->len - 1] != '/') out->len--;
                if (out->len > root) out->len--;
                out->data[out->len] = 0;
            }
        } else {
            if (start[n - 1] == '.' || start[n - 1] == ' ' || win_reserved(start, n)) return -1;
            if (out->data[out->len - 1] != '/') clay_str_push_char(out, '/');
            clay_str_push_n(out, start, n);
        }
    }
    return 0;
}

int clay_fs_resolve_windows_path(const char *workspace, const char *path, ClayStr *out) {
    ClayStr base, joined;
    int rc = -1;
    clay_str_init(out);
    if (win_normalize(workspace, &base) != 0) { clay_str_free(&base); return -1; }
    clay_str_init(&joined);
    if (win_alpha(path[0]) && path[1] == ':') {
        if (!win_sep(path[2])) goto done;
        clay_str_push(&joined, path);
    } else if (win_sep(path[0])) {
        if (!win_sep(path[1])) goto done;
        clay_str_push(&joined, path);
    } else {
        clay_str_push(&joined, base.data); clay_str_push_char(&joined, '/'); clay_str_push(&joined, path);
    }
    clay_str_free(out);
    if (win_normalize(joined.data, out) != 0) goto done;
    size_t n = base.len, i = 0;
    while (i < n && out->data[i] && base.data[i] == out->data[i]) i++;
    if (i == n && (base.data[n - 1] == '/' || out->data[n] == '/' || out->data[n] == 0)) rc = 0;
done:
    clay_str_free(&joined); clay_str_free(&base);
    return rc;
}

/* Resolves `path` (relative to the workspace, or absolute) to a normalized
   absolute path in `abs_out`. Returns 0 if the result stays inside
   `workspace_dir`, -1 if it would escape it. */
int clay_fs_resolve_workspace_path(const char *workspace_dir, const char *path, ClayStr *abs_out) {
#ifdef _WIN32
    ClayStr base;
    if (clay_fs_resolve_windows_path(workspace_dir, path, abs_out) != 0) return -1;
    if (win_normalize(workspace_dir, &base) != 0) { clay_str_free(&base); return -1; }
    size_t wlen = base.len;
    clay_str_free(&base);
#else
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
#endif
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

#ifdef __linux__
static int workspace_path(const char *absolute, int *root, const char **relative, char **cwd) {
    *cwd = clay_term_cwd();
    if (!*cwd) return -1;
    size_t n = strlen(*cwd);
    if (strncmp(absolute, *cwd, n) || (n != 1 && absolute[n] && absolute[n] != '/')) {
        free(*cwd); errno = EINVAL; return -1;
    }
    *relative = absolute + (n == 1 ? 1 : n);
    if (**relative == '/') ++*relative;
    *root = clay_workspace_fd_open();
    if (*root < 0) { free(*cwd); return -1; }
    return 0;
}

int clay_fs_read_workspace_limited(const char *path, size_t limit, ClayStr *out) {
    int root; const char *relative; char *cwd;
    if (workspace_path(path, &root, &relative, &cwd) != 0) return -1;
    int fd = clay_workspace_fd_read(root, relative);
    int error = errno;
    close(root); free(cwd); errno = error;
    if (fd < 0) return -1;
    clay_str_init(out);
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { error = errno; goto fail; }
        if (!n) break;
        if ((size_t)n > limit - out->len) { error = EFBIG; goto fail; }
        clay_str_push_n(out, buf, (size_t)n);
    }
    if (close(fd) < 0) { clay_str_free(out); return -1; }
    return 0;
fail:
    close(fd); clay_str_free(out); errno = error; return -1;
}

int clay_fs_write_workspace(const char *path, const void *data, size_t len) {
    int root; const char *relative; char *cwd;
    if (workspace_path(path, &root, &relative, &cwd) != 0) return -1;
    int rc = clay_workspace_fd_write(root, relative, data, len);
    int error = errno; close(root); free(cwd); errno = error;
    return rc;
}

int clay_fs_unlink_workspace(const char *path) {
    int root; const char *relative; char *cwd;
    if (workspace_path(path, &root, &relative, &cwd) != 0) return -1;
    int rc = clay_workspace_fd_unlink(root, relative);
    int error = errno; close(root); free(cwd); errno = error;
    return rc;
}
#endif

static int write_whole_file(const char *path, const char *data, size_t len) {
#ifdef __linux__
    return clay_fs_write_workspace(path, data, len);
#else
    return clay_term_write_project_file_atomic(path, data, len);
#endif
}

#ifndef __linux__
/* Creates every directory above `abs_path`'s final component. */
static int make_parent_dirs(const char *abs_path) {
    const char *slash = strrchr(abs_path, '/');
    if (!slash || slash == abs_path) return 1;
    ClayStr partial;
    clay_str_init(&partial);
    const char *p = abs_path;
#ifdef _WIN32
    if (win_alpha(p[0]) && p[1] == ':' && p[2] == '/') {
        clay_str_push_n(&partial, p, 3); p += 3;
    } else if (p[0] == '/' && p[1] == '/') {
        const char *server_end = strchr(p + 2, '/');
        const char *share_end = server_end ? strchr(server_end + 1, '/') : NULL;
        if (!share_end || share_end >= slash) { clay_str_free(&partial); return 1; }
        clay_str_push_n(&partial, p, (size_t)(share_end + 1 - p)); p = share_end + 1;
    } else
#endif
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

#endif

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
    if (fd >= 0 || (errno != ENOSYS && errno != EINVAL && errno != E2BIG)) return fd;
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
    size_t first_emitted = 0;
    int truncated = 0;
    int at_line_start = 1;
    int line_cut = 0;
    long page = limit > 0 ? limit : CLAY_FS_READ_DEFAULT_LINES;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (at_line_start) {
            line_no++;
            at_line_start = 0;
        }
        if (ch == '\n') {
            if (line_no > (size_t)(offset > 0 ? offset - 1 : 0) && emitted < (size_t)page) {
                if (!first_emitted) first_emitted = line_no;
                clay_str_printf(&formatted, "%5zu\t%s%s\n", line_no, line.data,
                                line_cut ? " [... line cut]" : "");
                emitted++;
            }
            clay_str_clear(&line);
            line_cut = 0;
            at_line_start = 1;
            if (formatted.len > CLAY_FS_READ_LIMIT) {
                truncated = 1;
                break;
            }
            continue;
        }
        /* Keep counting after the page is full so the total and the next
           offset stay right, but stop growing the line. */
        if (line.len < CLAY_FS_READ_LINE_LIMIT) clay_str_push_char(&line, (char)ch);
        else line_cut = 1;
    }
    if (!truncated && !at_line_start && line.len > 0 &&
        line_no > (size_t)(offset > 0 ? offset - 1 : 0) && emitted < (size_t)page) {
        if (!first_emitted) first_emitted = line_no;
        clay_str_printf(&formatted, "%5zu\t%s%s\n", line_no, line.data,
                        line_cut ? " [... line cut]" : "");
        emitted++;
    }
    clay_str_free(&line);
    fclose(file);

    if (emitted > 0 && emitted == (size_t)page && first_emitted + emitted <= line_no) {
        clay_str_printf(
            &formatted,
            "\n[Showing lines %zu-%zu of %zu. Use offset=%zu to continue.]\n",
            first_emitted, first_emitted + emitted - 1, line_no,
            first_emitted + emitted);
    }
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
    if (
#ifndef __linux__
        !make_parent_dirs(abs.data) ||
#endif
        write_whole_file(abs.data, content, strlen(content)) != 0) {
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
#ifdef __linux__
    if (clay_fs_read_workspace_limited(abs.data, CLAY_FS_READ_LIMIT, &content) != 0) {
#else
    if (clay_storage_read_limited(abs.data, CLAY_FS_READ_LIMIT, &content) != 0) {
#endif
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

#ifdef __linux__
void clay_fs_walk_files(const char *base_dir, const char *rel_prefix, const char *pattern, ClayArray *matches,
                      int *truncated) {
    if (*truncated || matches->count >= CLAY_FS_GLOB_MAX_MATCHES) { *truncated = 1; return; }
    char *workspace = clay_term_cwd();
    ClayStr abs;
    if (!workspace) return;
    if (clay_fs_resolve_workspace_path(workspace, base_dir, &abs) != 0) {
        clay_str_free(&abs);
        free(workspace);
        return;
    }
    size_t wlen = strlen(workspace);
    const char *relative = abs.data + (wlen == 1 ? 1 : wlen);
    if (*relative == '/') relative++;
    int cwd = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    int root = cwd < 0 ? -1 : read_open_beneath(cwd, relative, O_PATH | O_DIRECTORY);
    if (cwd >= 0) close(cwd);
    clay_str_free(&abs);
    free(workspace);
    if (root < 0) return;
    clay_fs_walk_files_fd(root, rel_prefix, pattern, matches, truncated);
    close(root);
}

void clay_fs_walk_files_fd(int root, const char *rel_prefix, const char *pattern, ClayArray *matches,
                           int *truncated) {
    if (*truncated || matches->count >= CLAY_FS_GLOB_MAX_MATCHES) { *truncated = 1; return; }
    ClayArray pending;
    clay_array_init(&pending, sizeof(char *));
    char *initial = strdup("");
    if (initial) clay_array_push_val(&pending, &initial);
    while (pending.count && !*truncated) {
        char *prefix = *(char **)clay_array_get(&pending, pending.count - 1);
        clay_array_remove(&pending, pending.count - 1);
        int fd = read_open_beneath(root, prefix, O_PATH);
        struct stat st;
        int valid = fd >= 0 && fstat(fd, &st) == 0 && !S_ISLNK(st.st_mode);
        if (valid && S_ISDIR(st.st_mode)) {
            int dirfd = read_open_beneath(root, prefix, O_RDONLY | O_DIRECTORY);
            struct stat dir_st;
            int same = dirfd >= 0 && fstat(dirfd, &dir_st) == 0 &&
                       st.st_dev == dir_st.st_dev && st.st_ino == dir_st.st_ino;
            close(fd);
            DIR *dir = same ? fdopendir(dirfd) : NULL;
            if (!dir && dirfd >= 0) close(dirfd);
            size_t first = pending.count;
            struct dirent *entry;
            while (dir && (entry = readdir(dir))) {
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
                    !strcmp(entry->d_name, ".git")) continue;
                ClayStr rel;
                clay_str_init(&rel);
                if (*prefix) { clay_str_push(&rel, prefix); clay_str_push_char(&rel, '/'); }
                clay_str_push(&rel, entry->d_name);
                char *copy = strdup(rel.data);
                if (copy) clay_array_push_val(&pending, &copy);
                clay_str_free(&rel);
            }
            if (dir) closedir(dir);
            for (size_t i = first, j = pending.count; i < j && i < --j; i++) {
                char **a = clay_array_get(&pending, i), **b = clay_array_get(&pending, j);
                char *tmp = *a; *a = *b; *b = tmp;
            }
        } else if (valid) {
            close(fd);
            ClayStr match;
            clay_str_init(&match);
            if (*rel_prefix) { clay_str_push(&match, rel_prefix); clay_str_push_char(&match, '/'); }
            clay_str_push(&match, prefix);
            if (clay_str_wildcard_match(pattern, match.data)) {
                if (matches->count >= CLAY_FS_GLOB_MAX_MATCHES) *truncated = 1;
                else {
                    char *copy = strdup(match.data);
                    if (copy) clay_array_push_val(matches, &copy);
                }
            }
            clay_str_free(&match);
        } else if (fd >= 0) {
            close(fd);
        }
        free(prefix);
    }
    for (size_t i = 0; i < pending.count; i++) free(*(char **)clay_array_get(&pending, i));
    clay_array_free(&pending);
}
#else
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

#endif

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

#ifdef __linux__
typedef int (*GrepFileFn)(int fd, const char *label, void *userdata);

typedef struct {
    const ClaySandboxConfig *sandbox;
    const char *base;
    const char *include;
    ClayStr *output;
    size_t limit;
    int rc;
    int exit_code;
    int ran;
    int truncated;
    int files_seen;
    time_t deadline;
} GrepFiles;

static int grep_include_matches(const char *include, const char *label) {
    for (const char *part = label; part; ) {
        if (fnmatch(include, part, 0) == 0) return 1;
        part = strchr(part, '/');
        if (part) part++;
    }
    return 0;
}

static int grep_run_fd(int fd, const char *label, void *userdata) {
    GrepFiles *files = userdata;
    if (time(NULL) >= files->deadline || files->files_seen >= CLAY_FS_GREP_MAX_FILES) {
        files->truncated = 1;
        return 1;
    }
    if (files->include && !grep_include_matches(files->include, label)) return 0;
    if (files->output->len >= files->limit) {
        files->truncated = 1;
        return 1;
    }
    files->files_seen++;
    ClayStr invocation;
    clay_str_init(&invocation);
    clay_str_push(&invocation, files->base);
    clay_str_push(&invocation, " --label=");
    clay_term_shell_quote(&invocation, label);
    clay_str_push(&invocation, " -- -");
    ClayExecResult exec = {0};
    int rc = clay_sandbox_exec_input(files->sandbox, invocation.data, files->output,
                                     files->limit, NULL, &exec, fd);
    clay_str_free(&invocation);
    files->ran = 1;
    if (rc != 0) files->rc = -1;
    if (exec.exit_code > files->exit_code) files->exit_code = exec.exit_code;
    if (exec.output_truncated) files->truncated = 1;
    return rc != 0 || exec.exit_code > 1 ? -1 : 0;
}

static int grep_walk_files(int root, const char *relative, GrepFileFn visit,
                           void *userdata, int depth) {
    if (depth > 24) {
        ((GrepFiles *)userdata)->truncated = 1;
        return 0;
    }
    int probe = read_open_beneath(root, relative, O_PATH);
    int dirfd = read_open_beneath(root, relative, O_RDONLY | O_DIRECTORY);
    struct stat probe_st, dir_st;
    int same = probe >= 0 && dirfd >= 0 &&
               fstat(probe, &probe_st) == 0 && fstat(dirfd, &dir_st) == 0 &&
               probe_st.st_dev == dir_st.st_dev && probe_st.st_ino == dir_st.st_ino;
    if (probe >= 0) close(probe);
    if (!same) {
        if (dirfd >= 0) close(dirfd);
        ((GrepFiles *)userdata)->truncated = 1;
        return 0;
    }
    DIR *dir = fdopendir(dirfd);
    if (!dir) { close(dirfd); return -1; }
    int rc = 0;
    int stop = 0;
    struct dirent *entry;
    while (!rc && !stop && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        ClayStr child;
        clay_str_init(&child);
        if (*relative) { clay_str_push(&child, relative); clay_str_push_char(&child, '/'); }
        clay_str_push(&child, entry->d_name);
        int fd = read_open_beneath(root, child.data, O_RDONLY | O_NONBLOCK);
        struct stat st;
        int stat_ok = fd >= 0 && fstat(fd, &st) == 0;
        if (stat_ok && S_ISDIR(st.st_mode)) {
            close(fd);
            int child_rc = grep_walk_files(root, child.data, visit, userdata, depth + 1);
            if (child_rc < 0) rc = -1;
            else if (child_rc > 0) stop = 1;
        } else if (stat_ok && S_ISREG(st.st_mode)) {
            int visit_rc = visit(fd, child.data, userdata);
            if (visit_rc < 0) rc = -1;
            else if (visit_rc > 0) stop = 1;
            close(fd);
        } else if (fd >= 0) {
            close(fd);
        }
        clay_str_free(&child);
    }
    closedir(dir);
    return rc < 0 ? -1 : 0;
}

static ClayJson *grep_result(ClayStr *output, int rc, int exit_code,
                             int truncated, ClayCommands *commands) {
    ClayJson *result = clay_json_object();
    int ok = rc == 0 && exit_code <= 1;
    clay_json_object_set(result, "ok", clay_json_bool(ok));
    if (exit_code == 1) {
        clay_json_object_set(result, "output", clay_json_string(""));
        clay_json_object_set(result, "output_truncated", clay_json_bool(0));
    } else if (output->len > CLAY_FS_GREP_LIMIT) {
        char *scratch_path = clay_chat_dump_scratch(commands->chat, "grep", output->data);
        ClayStr preview;
        clay_str_init(&preview);
        clay_str_push_n(&preview, output->data, CLAY_FS_GREP_LIMIT);
        if (scratch_path) clay_str_printf(&preview, "\n... (%zu bytes total, full output at %s)", output->len, scratch_path);
        else clay_str_push(&preview, "\n... (truncated)");
        clay_json_object_set(result, "output", clay_json_string(preview.data));
        clay_json_object_set(result, "output_truncated", clay_json_bool(1));
        if (scratch_path) clay_json_object_set(result, "scratch_path", clay_json_string(scratch_path));
        clay_str_free(&preview);
        free(scratch_path);
    } else {
        clay_json_object_set(result, "output", clay_json_string(output->data));
        clay_json_object_set(result, "output_truncated", clay_json_bool(truncated));
    }
    if (!ok) clay_json_object_set(result, "error", clay_json_string(rc != 0 ? "failed to start grep" : "grep failed"));
    return result;
}

static ClayJson *grep_linux_run(ClayCommands *commands, const char *pattern,
                                const char *include, const char *relative,
                                char *workspace_dir) {
    ClayStr base;
    clay_str_init(&base);
    clay_str_push(&base, "grep -Ern -I --binary-files=without-match -e ");
    clay_term_shell_quote(&base, pattern);
    ClayStr output;
    clay_str_init(&output);
    int root = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    int input_fd = -1;
    int input_open_failed = 0;
    if (root >= 0 && *relative) {
        input_fd = read_open_beneath(root, relative, O_RDONLY | O_NONBLOCK);
        struct stat st;
        int stat_ok = input_fd >= 0 && fstat(input_fd, &st) == 0;
        if (stat_ok && S_ISREG(st.st_mode)) {
            /* pinned regular file */
        } else {
            if (input_fd < 0 || !stat_ok) input_open_failed = 1;
            if (input_fd >= 0) close(input_fd);
            input_fd = -1;
        }
    } else if (!root && *relative) {
        input_open_failed = 1;
    }
    if (input_open_failed || (!root && !*relative)) {
        if (input_fd >= 0) close(input_fd);
        if (root >= 0) close(root);
        free(workspace_dir);
        clay_str_free(&base);
        clay_str_free(&output);
        ClayJson *error = clay_json_object();
        clay_json_object_set(error, "ok", clay_json_bool(0));
        clay_json_object_set(error, "output", clay_json_string(""));
        clay_json_object_set(error, "output_truncated", clay_json_bool(0));
        clay_json_object_set(error, "error", clay_json_string("grep failed"));
        return error;
    }
    char *scratch_dir = clay_chat_scratch_dir(commands->chat);
    ClaySandboxConfig sandbox = {
        .mode = commands->sandbox_mode,
        .shared = commands->sandbox_namespaces,
        .workspace_dir = workspace_dir,
        .scratch_dir = scratch_dir,
    };
    GrepFiles files = {
        .sandbox = &sandbox,
        .base = base.data,
        .include = include,
        .output = &output,
        .limit = CLAY_FS_GREP_CAPTURE_LIMIT,
        .exit_code = -1,
        .deadline = time(NULL) + 120,
    };
    int walk_rc = 0;
    if (input_fd >= 0) {
        int excluded = 0;
        if (include && *include && !grep_include_matches(include, relative)) excluded = 1;
        ClayStr invocation;
        clay_str_init(&invocation);
        clay_str_push(&invocation, base.data);
        if (include && *include) {
            clay_str_push(&invocation, " --include=");
            clay_term_shell_quote(&invocation, include);
        }
        clay_str_push(&invocation, " --label=");
        clay_term_shell_quote(&invocation, relative);
        clay_str_push(&invocation, " -- -");
        ClayExecResult exec = {0};
        files.rc = clay_sandbox_exec_input(&sandbox, invocation.data, &output,
                                           CLAY_FS_GREP_CAPTURE_LIMIT, NULL, &exec, input_fd);
        clay_str_free(&invocation);
        files.exit_code = exec.exit_code;
        files.truncated = exec.output_truncated;
        files.ran = 1;
        if (excluded && files.rc == 0 && files.exit_code <= 1) files.exit_code = 1;
        close(input_fd);
    } else {
        walk_rc = grep_walk_files(root, relative, grep_run_fd, &files, 0);
        if (walk_rc != 0) files.rc = -1;
        if (!files.ran) {
            int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (devnull >= 0) {
                grep_run_fd(devnull, "", &files);
                close(devnull);
            } else files.rc = -1;
        }
    }
    if (root >= 0) close(root);
    free(scratch_dir);
    free(workspace_dir);
    clay_str_free(&base);
    ClayJson *result = grep_result(&output, files.rc, files.exit_code,
                                   files.truncated, commands);
    clay_str_free(&output);
    return result;
}
#endif

ClayJson *clay_fs_tool_grep(const ClayJson *arguments, void *userdata) {
    ClayCommands *commands = userdata;
    const char *pattern = clay_json_string_value(clay_json_object_get(arguments, "pattern"));
    const char *path = clay_json_string_value(clay_json_object_get(arguments, "path"));
    const char *include = clay_json_string_value(clay_json_object_get(arguments, "glob"));
    if (include && !*include) include = NULL;
    if (!path || !*path) path = ".";
    ClayJson *result = clay_json_object();
#ifdef _WIN32
    /* shortcut: cmd.exe quoting is unsafe; use an argv-based executor to enable grep. */
    (void)commands; (void)pattern; (void)include;
    clay_json_object_set(result, "ok", clay_json_bool(0));
    clay_json_object_set(result, "error", clay_json_string("grep is unavailable on Windows"));
    return result;
#endif
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
    const char *relative = target.data + strlen(workspace_dir);
    while (*relative == '/') relative++;
#ifdef __linux__
    {
        char *relative_copy = strdup(relative);
        clay_str_free(&target);
        if (!relative_copy) {
            free(workspace_dir);
            clay_json_object_set(result, "ok", clay_json_bool(0));
            clay_json_object_set(result, "error", clay_json_string("grep failed"));
            return result;
        }
        ClayJson *linux_result = grep_linux_run(commands, pattern, include,
                                                relative_copy, workspace_dir);
        free(relative_copy);
        clay_json_object_set(linux_result, "pattern", clay_json_string(pattern));
        clay_json_free(result);
        return linux_result;
    }
#else
    ClayStr invocation;
    clay_str_init(&invocation);
    clay_str_push(&invocation, "grep -Ern -I --binary-files=without-match -e ");
    clay_term_shell_quote(&invocation, pattern);
    if (include && *include) {
        clay_str_push(&invocation, " --include=");
        clay_term_shell_quote(&invocation, include);
    }
#ifdef _WIN32
    ClayStr normalized_workspace;
    win_normalize(workspace_dir, &normalized_workspace);
    const char *relative_target = target.data + normalized_workspace.len;
    clay_str_free(&normalized_workspace);
#else
    const char *relative_target = target.data + strlen(workspace_dir);
#endif
    clay_str_push(&invocation, " -- ");
    clay_term_shell_quote(&invocation, *relative_target ? relative_target : ".");
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
    int rc = clay_sandbox_exec(&sandbox, invocation.data, &output,
                               CLAY_FS_GREP_CAPTURE_LIMIT, NULL, &exec);
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
#endif
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
