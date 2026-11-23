#include "clay/json.h"

#include "clay/array.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;
    ClayJson *value;
} ClayJsonMember;

struct ClayJson {
    ClayJsonType type;
    int boolean;
    double number;
    char *string;
    ClayArray items;   /* ClayJson*, for CLAY_JSON_ARRAY */
    ClayArray members; /* ClayJsonMember, for CLAY_JSON_OBJECT */
};

static int json_fail_after = -1;

/* Test seam: fail the Nth json_new allocation (0 = next), then reset. */
void clay_json_test_fail_allocation(int after) {
    json_fail_after = after;
}

static ClayJson *json_new(ClayJsonType type) {
    if (json_fail_after == 0) {
        json_fail_after = -1;
        return NULL;
    }
    if (json_fail_after > 0) json_fail_after--;
    ClayJson *v = calloc(1, sizeof(ClayJson));
    if (!v) return NULL;
    v->type = type;
    return v;
}

ClayJson *clay_json_null(void) {
    return json_new(CLAY_JSON_NULL);
}

ClayJson *clay_json_bool(int value) {
    ClayJson *v = json_new(CLAY_JSON_BOOL);
    if (!v) return NULL;
    v->boolean = value ? 1 : 0;
    return v;
}

ClayJson *clay_json_number(double value) {
    ClayJson *v = json_new(CLAY_JSON_NUMBER);
    if (!v) return NULL;
    v->number = value;
    return v;
}

ClayJson *clay_json_string(const char *value) {
    ClayJson *v = json_new(CLAY_JSON_STRING);
    if (!v) return NULL;
    v->string = strdup(value ? value : "");
    if (!v->string) {
        free(v);
        return NULL;
    }
    return v;
}

char *clay_json_take_string(ClayJson *value) {
    if (!value || value->type != CLAY_JSON_STRING) return NULL;
    char *string = value->string;
    value->string = NULL;
    value->type = CLAY_JSON_NULL;
    return string;
}

ClayJson *clay_json_array(void) {
    ClayJson *v = json_new(CLAY_JSON_ARRAY);
    if (!v) return NULL;
    clay_array_init(&v->items, sizeof(ClayJson *));
    return v;
}

ClayJson *clay_json_object(void) {
    ClayJson *v = json_new(CLAY_JSON_OBJECT);
    if (!v) return NULL;
    clay_array_init(&v->members, sizeof(ClayJsonMember));
    return v;
}

static int json_array_push(ClayJson *array, ClayJson *value) {
    if (!array || array->type != CLAY_JSON_ARRAY || !value) return 0;
    void *slot = clay_array_push(&array->items);
    if (!slot) return 0;
    *(ClayJson **)slot = value;
    return 1;
}

void clay_json_array_push(ClayJson *array, ClayJson *value) {
    (void)json_array_push(array, value);
}

void clay_json_array_remove(ClayJson *array, size_t index) {
    if (index >= array->items.count) return;
    clay_json_free(*(ClayJson **)clay_array_get(&array->items, index));
    clay_array_remove(&array->items, index);
}

static int json_object_set(ClayJson *object, const char *key, ClayJson *value) {
    if (!object || object->type != CLAY_JSON_OBJECT || !key) {
        clay_json_free(value);
        return 0;
    }
    for (size_t i = 0; i < object->members.count; i++) {
        ClayJsonMember *m = clay_array_get(&object->members, i);
        if (strcmp(m->key, key) == 0) {
            clay_json_free(m->value);
            m->value = value;
            return 1;
        }
    }
    ClayJsonMember m;
    m.key = strdup(key);
    if (!m.key) {
        clay_json_free(value);
        return 0;
    }
    m.value = value;
    ClayJsonMember *slot = clay_array_push(&object->members);
    if (!slot) {
        free(m.key);
        clay_json_free(value);
        return 0;
    }
    *slot = m;
    return 1;
}

void clay_json_object_set(ClayJson *object, const char *key, ClayJson *value) {
    (void)json_object_set(object, key, value);
}

void clay_json_free(ClayJson *value) {
    if (!value) return;

    switch (value->type) {
        case CLAY_JSON_STRING:
            free(value->string);
            break;
        case CLAY_JSON_ARRAY:
            for (size_t i = 0; i < value->items.count; i++) {
                clay_json_free(*(ClayJson **)clay_array_get(&value->items, i));
            }
            clay_array_free(&value->items);
            break;
        case CLAY_JSON_OBJECT:
            for (size_t i = 0; i < value->members.count; i++) {
                ClayJsonMember *m = clay_array_get(&value->members, i);
                free(m->key);
                clay_json_free(m->value);
            }
            clay_array_free(&value->members);
            break;
        default:
            break;
    }
    free(value);
}

ClayJson *clay_json_clone(const ClayJson *value) {
    if (!value) return NULL;

    switch (value->type) {
        case CLAY_JSON_NULL:
            return clay_json_null();
        case CLAY_JSON_BOOL:
            return clay_json_bool(value->boolean);
        case CLAY_JSON_NUMBER:
            return clay_json_number(value->number);
        case CLAY_JSON_STRING:
            return clay_json_string(value->string);
        case CLAY_JSON_ARRAY: {
            ClayJson *arr = clay_json_array();
            if (!arr) return NULL;
            for (size_t i = 0; i < value->items.count; i++) {
                ClayJson *item = *(ClayJson **)clay_array_get((ClayArray *)&value->items, i);
                ClayJson *copy = clay_json_clone(item);
                if (!copy || !json_array_push(arr, copy)) {
                    clay_json_free(copy);
                    clay_json_free(arr);
                    return NULL;
                }
            }
            return arr;
        }
        case CLAY_JSON_OBJECT: {
            ClayJson *obj = clay_json_object();
            if (!obj) return NULL;
            for (size_t i = 0; i < value->members.count; i++) {
                ClayJsonMember *m = clay_array_get((ClayArray *)&value->members, i);
                ClayJson *copy = clay_json_clone(m->value);
                if (!copy) {
                    clay_json_free(obj);
                    return NULL;
                }
                if (!json_object_set(obj, m->key, copy)) {
                    clay_json_free(obj);
                    return NULL;
                }
            }
            return obj;
        }
    }
    return clay_json_null();
}

ClayJsonType clay_json_type(const ClayJson *value) {
    return value ? value->type : CLAY_JSON_NULL;
}

int clay_json_bool_value(const ClayJson *value) {
    return value && value->type == CLAY_JSON_BOOL ? value->boolean : 0;
}

double clay_json_number_value(const ClayJson *value) {
    return value && value->type == CLAY_JSON_NUMBER ? value->number : 0.0;
}

const char *clay_json_string_value(const ClayJson *value) {
    return value && value->type == CLAY_JSON_STRING ? value->string : "";
}

size_t clay_json_array_count(const ClayJson *value) {
    return value && value->type == CLAY_JSON_ARRAY ? value->items.count : 0;
}

ClayJson *clay_json_array_get(const ClayJson *value, size_t index) {
    if (!value || value->type != CLAY_JSON_ARRAY || index >= value->items.count) return NULL;
    return *(ClayJson **)clay_array_get((ClayArray *)&value->items, index);
}

ClayJson *clay_json_object_get(const ClayJson *value, const char *key) {
    if (!value || value->type != CLAY_JSON_OBJECT) return NULL;
    for (size_t i = 0; i < value->members.count; i++) {
        ClayJsonMember *m = clay_array_get((ClayArray *)&value->members, i);
        if (strcmp(m->key, key) == 0) return m->value;
    }
    return NULL;
}

static void json_escape_string(const char *s, ClayStr *out) {
    clay_str_push_char(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': clay_str_push(out, "\\\""); break;
            case '\\': clay_str_push(out, "\\\\"); break;
            case '\n': clay_str_push(out, "\\n"); break;
            case '\r': clay_str_push(out, "\\r"); break;
            case '\t': clay_str_push(out, "\\t"); break;
            case '\b': clay_str_push(out, "\\b"); break;
            case '\f': clay_str_push(out, "\\f"); break;
            default:
                if (*p < 0x20) {
                    clay_str_printf(out, "\\u%04x", *p);
                } else {
                    clay_str_push_char(out, (char)*p);
                }
        }
    }
    clay_str_push_char(out, '"');
}

void clay_json_stringify(const ClayJson *value, ClayStr *out) {
    if (!value) {
        clay_str_push(out, "null");
        return;
    }

    switch (value->type) {
        case CLAY_JSON_NULL:
            clay_str_push(out, "null");
            break;
        case CLAY_JSON_BOOL:
            clay_str_push(out, value->boolean ? "true" : "false");
            break;
        case CLAY_JSON_NUMBER:
            if (value->number == (double)(long long)value->number) {
                clay_str_printf(out, "%lld", (long long)value->number);
            } else {
                clay_str_printf(out, "%g", value->number);
            }
            break;
        case CLAY_JSON_STRING:
            json_escape_string(value->string, out);
            break;
        case CLAY_JSON_ARRAY:
            clay_str_push_char(out, '[');
            for (size_t i = 0; i < value->items.count; i++) {
                if (i > 0) clay_str_push_char(out, ',');
                clay_json_stringify(*(ClayJson **)clay_array_get((ClayArray *)&value->items, i), out);
            }
            clay_str_push_char(out, ']');
            break;
        case CLAY_JSON_OBJECT:
            clay_str_push_char(out, '{');
            for (size_t i = 0; i < value->members.count; i++) {
                if (i > 0) clay_str_push_char(out, ',');
                ClayJsonMember *m = clay_array_get((ClayArray *)&value->members, i);
                json_escape_string(m->key, out);
                clay_str_push_char(out, ':');
                clay_json_stringify(m->value, out);
            }
            clay_str_push_char(out, '}');
            break;
    }
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

#define CLAY_JSON_MAX_DEPTH 128

static int parse_hex4(const char **pp, unsigned int *value) {
    const char *p = *pp;
    unsigned int result = 0;
    for (int i = 0; i < 4; i++, p++) {
        char c = *p;
        unsigned int digit;
        if (c >= '0' && c <= '9') digit = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned int)(c - 'A' + 10);
        else return 0;
        result = (result << 4) | digit;
    }
    *pp = p;
    *value = result;
    return 1;
}

static int json_push_char(ClayStr *s, char c) {
    size_t before = s->len;
    clay_str_push_char(s, c);
    return s->len == before + 1;
}

static int append_utf8(ClayStr *s, unsigned int cp) {
    if (cp < 0x80)
        return json_push_char(s, (char)cp);
    if (cp < 0x800)
        return json_push_char(s, (char)(0xC0 | (cp >> 6))) &&
               json_push_char(s, (char)(0x80 | (cp & 0x3F)));
    if (cp < 0x10000)
        return json_push_char(s, (char)(0xE0 | (cp >> 12))) &&
               json_push_char(s, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
               json_push_char(s, (char)(0x80 | (cp & 0x3F)));
    return json_push_char(s, (char)(0xF0 | (cp >> 18))) &&
           json_push_char(s, (char)(0x80 | ((cp >> 12) & 0x3F))) &&
           json_push_char(s, (char)(0x80 | ((cp >> 6) & 0x3F))) &&
           json_push_char(s, (char)(0x80 | (cp & 0x3F)));
}

/* Parses a JSON string starting at *pp (which must point at the opening
   quote), advances *pp past the closing quote. Returns a malloc'd,
   unescaped C string, or NULL on a malformed literal. */
static char *parse_string_raw(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;

    ClayStr s;
    clay_str_init(&s);

    while (*p && *p != '"') {
        if (*p != '\\') {
            if ((unsigned char)*p < 0x20) {
                clay_str_free(&s);
                return NULL;
            }
            if (!json_push_char(&s, *p)) {
                clay_str_free(&s);
                return NULL;
            }
            p++;
            continue;
        }

        p++;
        switch (*p) {
            case '"': if (!json_push_char(&s, '"')) { clay_str_free(&s); return NULL; } p++; break;
            case '\\': if (!json_push_char(&s, '\\')) { clay_str_free(&s); return NULL; } p++; break;
            case '/': if (!json_push_char(&s, '/')) { clay_str_free(&s); return NULL; } p++; break;
            case 'n': if (!json_push_char(&s, '\n')) { clay_str_free(&s); return NULL; } p++; break;
            case 't': if (!json_push_char(&s, '\t')) { clay_str_free(&s); return NULL; } p++; break;
            case 'r': if (!json_push_char(&s, '\r')) { clay_str_free(&s); return NULL; } p++; break;
            case 'b': if (!json_push_char(&s, '\b')) { clay_str_free(&s); return NULL; } p++; break;
            case 'f': if (!json_push_char(&s, '\f')) { clay_str_free(&s); return NULL; } p++; break;
            case 'u': {
                p++;
                unsigned int cp = 0;
                if (!parse_hex4(&p, &cp)) {
                    clay_str_free(&s);
                    return NULL;
                }
                if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    clay_str_free(&s);
                    return NULL;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p[0] != '\\' || p[1] != 'u') {
                        clay_str_free(&s);
                        return NULL;
                    }
                    p += 2;
                    unsigned int low = 0;
                    if (!parse_hex4(&p, &low) || low < 0xDC00 || low > 0xDFFF) {
                        clay_str_free(&s);
                        return NULL;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                if (!append_utf8(&s, cp)) {
                    clay_str_free(&s);
                    return NULL;
                }
                break;
            }
            default:
                clay_str_free(&s);
                return NULL;
        }
    }

    if (*p != '"') {
        clay_str_free(&s);
        return NULL;
    }
    p++;

    if (!s.data) {
        clay_str_free(&s);
        return NULL;
    }
    *pp = p;
    return s.data;
}

static ClayJson *parse_value(const char **pp, unsigned int depth) {
    const char *p = skip_ws(*pp);
    if (depth > CLAY_JSON_MAX_DEPTH) return NULL;

    if (strncmp(p, "null", 4) == 0) { *pp = p + 4; return clay_json_null(); }
    if (strncmp(p, "true", 4) == 0) { *pp = p + 4; return clay_json_bool(1); }
    if (strncmp(p, "false", 5) == 0) { *pp = p + 5; return clay_json_bool(0); }

    if (*p == '"') {
        char *s = parse_string_raw(&p);
        if (!s) return NULL;
        ClayJson *v = json_new(CLAY_JSON_STRING);
        if (!v) {
            free(s);
            return NULL;
        }
        v->string = s;
        *pp = p;
        return v;
    }

    if (*p == '[') {
        p++;
        ClayJson *arr = clay_json_array();
        if (!arr) return NULL;
        p = skip_ws(p);
        if (*p == ']') {
            p++;
            *pp = p;
            return arr;
        }
        for (;;) {
            ClayJson *item = parse_value(&p, depth + 1);
            if (!item) {
                clay_json_free(arr);
                return NULL;
            }
            if (!json_array_push(arr, item)) {
                clay_json_free(item);
                clay_json_free(arr);
                return NULL;
            }
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; break; }
            clay_json_free(arr);
            return NULL;
        }
        *pp = p;
        return arr;
    }

    if (*p == '{') {
        p++;
        ClayJson *obj = clay_json_object();
        if (!obj) return NULL;
        p = skip_ws(p);
        if (*p == '}') {
            p++;
            *pp = p;
            return obj;
        }
        for (;;) {
            p = skip_ws(p);
            char *key = parse_string_raw(&p);
            if (!key) {
                clay_json_free(obj);
                return NULL;
            }
            p = skip_ws(p);
            if (*p != ':') {
                free(key);
                clay_json_free(obj);
                return NULL;
            }
            p++;
            ClayJson *val = parse_value(&p, depth + 1);
            if (!val) {
                free(key);
                clay_json_free(obj);
                return NULL;
            }
            if (!json_object_set(obj, key, val)) {
                free(key);
                clay_json_free(obj);
                return NULL;
            }
            free(key);
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            clay_json_free(obj);
            return NULL;
        }
        *pp = p;
        return obj;
    }

    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *end;
        double n = strtod(p, &end);
        if (end == p) return NULL;
        *pp = end;
        return clay_json_number(n);
    }

    return NULL;
}

ClayJson *clay_json_parse(const char *text, const char **end_out) {
    if (!text) return NULL;
    const char *p = text;
    ClayJson *v = parse_value(&p, 0);
    if (!v) return NULL;

    p = skip_ws(p);
    if (*p != '\0') {
        clay_json_free(v);
        return NULL;
    }
    if (end_out) *end_out = p;
    return v;
}
