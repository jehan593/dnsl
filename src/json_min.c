#include "json_min.h"
#include <string.h>
#include <stdlib.h>

/* ---------- constructors ---------- */

JsonValue *json_new_null(void) {
    JsonValue *v = g_new0(JsonValue, 1);
    v->type = JSON_NULL;
    return v;
}

JsonValue *json_new_bool(gboolean b) {
    JsonValue *v = g_new0(JsonValue, 1);
    v->type = JSON_BOOL;
    v->v.b = b;
    return v;
}

JsonValue *json_new_number(gint64 n) {
    JsonValue *v = g_new0(JsonValue, 1);
    v->type = JSON_NUMBER;
    v->v.n = n;
    return v;
}

JsonValue *json_new_string(const gchar *s) {
    if (!s) return json_new_null();
    JsonValue *v = g_new0(JsonValue, 1);
    v->type = JSON_STRING;
    v->v.s = g_strdup(s);
    return v;
}

JsonValue *json_new_array(void) {
    JsonValue *v = g_new0(JsonValue, 1);
    v->type = JSON_ARRAY;
    v->v.arr = g_ptr_array_new();
    return v;
}

JsonValue *json_new_object(void) {
    JsonValue *v = g_new0(JsonValue, 1);
    v->type = JSON_OBJECT;
    v->v.obj = g_ptr_array_new();
    return v;
}

void json_array_append(JsonValue *array, JsonValue *item) {
    g_return_if_fail(array != NULL && array->type == JSON_ARRAY);
    g_ptr_array_add(array->v.arr, item);
}

void json_object_set(JsonValue *object, const gchar *key, JsonValue *value) {
    g_return_if_fail(object != NULL && object->type == JSON_OBJECT);
    JsonMember *m = g_new0(JsonMember, 1);
    m->key = g_strdup(key);
    m->value = value;
    g_ptr_array_add(object->v.obj, m);
}

JsonValue *json_object_get(const JsonValue *object, const gchar *key) {
    if (!object || object->type != JSON_OBJECT) return NULL;
    for (guint i = 0; i < object->v.obj->len; i++) {
        JsonMember *m = g_ptr_array_index(object->v.obj, i);
        if (g_strcmp0(m->key, key) == 0) return m->value;
    }
    return NULL;
}

const gchar *json_get_string(const JsonValue *object, const gchar *key, const gchar *default_value) {
    JsonValue *v = json_object_get(object, key);
    if (!v || v->type != JSON_STRING) return default_value;
    return v->v.s;
}

gint64 json_get_int(const JsonValue *object, const gchar *key, gint64 default_value) {
    JsonValue *v = json_object_get(object, key);
    if (!v || v->type != JSON_NUMBER) return default_value;
    return v->v.n;
}

gboolean json_get_bool(const JsonValue *object, const gchar *key, gboolean default_value) {
    JsonValue *v = json_object_get(object, key);
    if (!v || v->type != JSON_BOOL) return default_value;
    return v->v.b;
}

void json_value_free(JsonValue *value) {
    if (!value) return;
    switch (value->type) {
        case JSON_STRING:
            g_free(value->v.s);
            break;
        case JSON_ARRAY:
            for (guint i = 0; i < value->v.arr->len; i++) {
                json_value_free(g_ptr_array_index(value->v.arr, i));
            }
            g_ptr_array_free(value->v.arr, TRUE);
            break;
        case JSON_OBJECT:
            for (guint i = 0; i < value->v.obj->len; i++) {
                JsonMember *m = g_ptr_array_index(value->v.obj, i);
                g_free(m->key);
                json_value_free(m->value);
                g_free(m);
            }
            g_ptr_array_free(value->v.obj, TRUE);
            break;
        default:
            break;
    }
    g_free(value);
}

/* ---------- parser ---------- */

typedef struct {
    const gchar *p;
    const gchar *end;
} Cursor;

static void skip_ws(Cursor *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static JsonValue *parse_value(Cursor *c, GError **error);

static gboolean expect_char(Cursor *c, char ch, GError **error) {
    skip_ws(c);
    if (c->p >= c->end || *c->p != ch) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "expected '%c' at offset %ld", ch, (long) (c->p - c->end));
        return FALSE;
    }
    c->p++;
    return TRUE;
}

static gchar *parse_string_raw(Cursor *c, GError **error) {
    if (!expect_char(c, '"', error)) return NULL;
    GString *out = g_string_new(NULL);
    while (c->p < c->end && *c->p != '"') {
        gchar ch = *c->p;
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) break;
            switch (*c->p) {
                case '"': g_string_append_c(out, '"'); break;
                case '\\': g_string_append_c(out, '\\'); break;
                case '/': g_string_append_c(out, '/'); break;
                case 'b': g_string_append_c(out, '\b'); break;
                case 'f': g_string_append_c(out, '\f'); break;
                case 'n': g_string_append_c(out, '\n'); break;
                case 'r': g_string_append_c(out, '\r'); break;
                case 't': g_string_append_c(out, '\t'); break;
                case 'u': {
                    if (c->p + 4 < c->end) {
                        gchar hex[5] = {c->p[1], c->p[2], c->p[3], c->p[4], 0};
                        gunichar cp = (gunichar) g_ascii_strtoll(hex, NULL, 16);
                        g_string_append_unichar(out, cp);
                        c->p += 4;
                    }
                    break;
                }
                default: g_string_append_c(out, *c->p); break;
            }
            c->p++;
        } else {
            g_string_append_c(out, ch);
            c->p++;
        }
    }
    if (c->p >= c->end || *c->p != '"') {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "unterminated string");
        g_string_free(out, TRUE);
        return NULL;
    }
    c->p++; /* closing quote */
    return g_string_free(out, FALSE);
}

static JsonValue *parse_object(Cursor *c, GError **error) {
    if (!expect_char(c, '{', error)) return NULL;
    JsonValue *obj = json_new_object();
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return obj;
    }
    while (TRUE) {
        skip_ws(c);
        gchar *key = parse_string_raw(c, error);
        if (!key) {
            json_value_free(obj);
            return NULL;
        }
        if (!expect_char(c, ':', error)) {
            g_free(key);
            json_value_free(obj);
            return NULL;
        }
        skip_ws(c);
        JsonValue *val = parse_value(c, error);
        if (!val) {
            g_free(key);
            json_value_free(obj);
            return NULL;
        }
        json_object_set(obj, key, val);
        g_free(key);
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        break;
    }
    if (!expect_char(c, '}', error)) {
        json_value_free(obj);
        return NULL;
    }
    return obj;
}

static JsonValue *parse_array(Cursor *c, GError **error) {
    if (!expect_char(c, '[', error)) return NULL;
    JsonValue *arr = json_new_array();
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return arr;
    }
    while (TRUE) {
        skip_ws(c);
        JsonValue *val = parse_value(c, error);
        if (!val) {
            json_value_free(arr);
            return NULL;
        }
        json_array_append(arr, val);
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        break;
    }
    if (!expect_char(c, ']', error)) {
        json_value_free(arr);
        return NULL;
    }
    return arr;
}

static JsonValue *parse_value(Cursor *c, GError **error) {
    skip_ws(c);
    if (c->p >= c->end) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "unexpected end of input");
        return NULL;
    }
    char ch = *c->p;
    if (ch == '{') return parse_object(c, error);
    if (ch == '[') return parse_array(c, error);
    if (ch == '"') {
        gchar *s = parse_string_raw(c, error);
        if (!s) return NULL;
        JsonValue *v = json_new_string(s);
        g_free(s);
        return v;
    }
    if (strncmp(c->p, "true", 4) == 0) {
        c->p += 4;
        return json_new_bool(TRUE);
    }
    if (strncmp(c->p, "false", 5) == 0) {
        c->p += 5;
        return json_new_bool(FALSE);
    }
    if (strncmp(c->p, "null", 4) == 0) {
        c->p += 4;
        return json_new_null();
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        const gchar *start = c->p;
        if (*c->p == '-') c->p++;
        while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' || *c->p == 'e' || *c->p == 'E' || *c->p == '+' || *c->p == '-')) {
            c->p++;
        }
        gchar *numstr = g_strndup(start, c->p - start);
        gint64 n = g_ascii_strtoll(numstr, NULL, 10);
        g_free(numstr);
        return json_new_number(n);
    }
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "unexpected character '%c'", ch);
    return NULL;
}

JsonValue *json_parse(const gchar *text, GError **error) {
    if (!text) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "empty input");
        return NULL;
    }
    Cursor c = {text, text + strlen(text)};
    JsonValue *v = parse_value(&c, error);
    return v;
}

/* ---------- writer ---------- */

static void write_string_escaped(GString *out, const gchar *s) {
    g_string_append_c(out, '"');
    for (const gchar *p = s; *p; p++) {
        switch (*p) {
            case '"': g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n"); break;
            case '\r': g_string_append(out, "\\r"); break;
            case '\t': g_string_append(out, "\\t"); break;
            default:
                if ((guchar) *p < 0x20) {
                    g_string_append_printf(out, "\\u%04x", *p);
                } else {
                    g_string_append_c(out, *p);
                }
        }
    }
    g_string_append_c(out, '"');
}

static void write_value(GString *out, const JsonValue *value, int indent) {
    if (!value) {
        g_string_append(out, "null");
        return;
    }
    switch (value->type) {
        case JSON_NULL:
            g_string_append(out, "null");
            break;
        case JSON_BOOL:
            g_string_append(out, value->v.b ? "true" : "false");
            break;
        case JSON_NUMBER:
            g_string_append_printf(out, "%" G_GINT64_FORMAT, value->v.n);
            break;
        case JSON_STRING:
            write_string_escaped(out, value->v.s);
            break;
        case JSON_ARRAY: {
            if (value->v.arr->len == 0) {
                g_string_append(out, "[]");
                break;
            }
            g_string_append(out, "[\n");
            for (guint i = 0; i < value->v.arr->len; i++) {
                for (int j = 0; j < indent + 1; j++) g_string_append(out, "  ");
                write_value(out, g_ptr_array_index(value->v.arr, i), indent + 1);
                if (i + 1 < value->v.arr->len) g_string_append_c(out, ',');
                g_string_append_c(out, '\n');
            }
            for (int j = 0; j < indent; j++) g_string_append(out, "  ");
            g_string_append_c(out, ']');
            break;
        }
        case JSON_OBJECT: {
            if (value->v.obj->len == 0) {
                g_string_append(out, "{}");
                break;
            }
            g_string_append(out, "{\n");
            for (guint i = 0; i < value->v.obj->len; i++) {
                JsonMember *m = g_ptr_array_index(value->v.obj, i);
                for (int j = 0; j < indent + 1; j++) g_string_append(out, "  ");
                write_string_escaped(out, m->key);
                g_string_append(out, ": ");
                write_value(out, m->value, indent + 1);
                if (i + 1 < value->v.obj->len) g_string_append_c(out, ',');
                g_string_append_c(out, '\n');
            }
            for (int j = 0; j < indent; j++) g_string_append(out, "  ");
            g_string_append_c(out, '}');
            break;
        }
    }
}

gchar *json_to_string(const JsonValue *value) {
    GString *out = g_string_new(NULL);
    write_value(out, value, 0);
    return g_string_free(out, FALSE);
}

gchar *json_to_string_compact(const JsonValue *value) {
    gchar *pretty = json_to_string(value);

    GString *out = g_string_new(NULL);
    gboolean in_string = FALSE, escaped = FALSE;
    for (const gchar *p = pretty; *p; p++) {
        gchar ch = *p;
        if (in_string) {
            g_string_append_c(out, ch);
            if (escaped) escaped = FALSE;
            else if (ch == '\\') escaped = TRUE;
            else if (ch == '"') in_string = FALSE;
            continue;
        }
        if (ch == '"') { in_string = TRUE; g_string_append_c(out, ch); continue; }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue; /* unambiguous without it */
        g_string_append_c(out, ch);
    }
    g_free(pretty);
    return g_string_free(out, FALSE);
}
