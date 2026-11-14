#define _GNU_SOURCE

#include "clay/array.h"
#include "clay/json.h"
#include "clay/storage.h"
#include "clay/term.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <endian.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <sys/xattr.h>
#endif

extern char *mkdtemp(char *template);

int main(void) {
    char template[] = "/tmp/clay_test_storage_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);

    char *root = clay_storage_root();
    assert(root);
    assert(strstr(root, "/.clay") != NULL);
    free(root);

    assert(clay_storage_ensure_dir("providers/nested") == 0);
    char *path = clay_storage_path("providers/nested/config.json");
    assert(path);
    assert(clay_storage_write_atomic_private(path, "hello", 5) == 0);

    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(chmod(path, 0755) == 0);
    assert(clay_storage_write_atomic_private(path, "hello", 5) == 0);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    ClayStr body;
    assert(clay_storage_read_limited(path, 5, &body) == 0);
    assert(body.len == 5 && memcmp(body.data, "hello", 5) == 0);
    clay_str_free(&body);
    assert(clay_storage_read_limited(path, 4, &body) != 0);
    assert(errno == EFBIG);

    ClayJson *json = clay_json_object();
    clay_json_object_set(json, "name", clay_json_string("clay"));
    assert(clay_storage_write_json_atomic_private(path, json) == 0);
    clay_json_free(json);
    json = clay_storage_read_json(path, 1024);
    assert(json);
    assert(strcmp(clay_json_string_value(clay_json_object_get(json, "name")),
                  "clay") == 0);
    clay_json_free(json);
    assert(clay_storage_write_atomic_private(path, "{", 1) == 0);
    assert(clay_storage_read_json(path, 1024) == NULL);
#ifdef __linux__
    struct {
        struct posix_acl_xattr_header header;
        struct posix_acl_xattr_entry entries[5];
    } acl = {0};
    acl.header.a_version = htole32(POSIX_ACL_XATTR_VERSION);
    const int tags[] = {ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_MASK, ACL_OTHER};
    const int perms[] = {6, 4, 0, 4, 0};
    for (int i = 0; i < 5; ++i) {
        acl.entries[i].e_tag = htole16(tags[i]);
        acl.entries[i].e_perm = htole16(perms[i]);
        acl.entries[i].e_id = htole32(i == 1 ? 12345 : ACL_UNDEFINED_ID);
    }
    if (setxattr(path, "system.posix_acl_access", &acl, sizeof(acl), 0) == 0) {
        assert(clay_term_write_project_file_atomic(path, "changed", 7) != 0);
        assert(clay_storage_read_limited(path, 1024, &body) == 0);
        assert(body.len == 1 && body.data[0] == '{');
        clay_str_free(&body);
        assert(getxattr(path, "system.posix_acl_access", NULL, 0) > 0);
        assert(removexattr(path, "system.posix_acl_access") == 0);
        char directory[512];
        snprintf(directory, sizeof(directory), "%s/.clay/providers/nested", home);
        assert(setxattr(directory, "system.posix_acl_default", &acl, sizeof(acl), 0) == 0);
        assert(clay_term_write_project_file_atomic(path, "changed", 7) != 0);
        assert(clay_storage_read_limited(path, 1024, &body) == 0);
        assert(body.len == 1 && body.data[0] == '{');
        clay_str_free(&body);
        assert(clay_storage_write_atomic_private(path, "private", 7) == 0);
        assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
        assert(removexattr(directory, "system.posix_acl_default") == 0);
    } else {
        assert(errno == ENOTSUP || errno == EOPNOTSUPP);
    }
#endif
    remove(path);
    free(path);

    ClayStr overflow = {0};
    overflow.data = malloc(1);
    overflow.cap = 1;
    overflow.len = SIZE_MAX;
    clay_str_push_char(&overflow, 'x');
    assert(overflow.len == SIZE_MAX);
    clay_str_free(&overflow);

    ClayArray array = {0};
    array.elem_size = 2;
    array.count = SIZE_MAX;
    array.cap = SIZE_MAX;
    assert(clay_array_push(&array) == NULL);
    free(array.data);

    assert(chdir("/tmp") == 0);
    char nested[512], providers[512], clay[512];
    snprintf(nested, sizeof(nested), "%s/.clay/providers/nested", home);
    snprintf(providers, sizeof(providers), "%s/.clay/providers", home);
    snprintf(clay, sizeof(clay), "%s/.clay", home);
    assert(rmdir(nested) == 0);
    assert(rmdir(providers) == 0);
    assert(rmdir(clay) == 0);
    assert(rmdir(home) == 0);
    printf("storage tests passed\n");
    return 0;
}
