/* Minimal hand-rolled JSON reader/writer for Linker's own small, fully self-controlled
 * schema (two flat entity arrays + a couple of settings fields). Kept dependency-free
 * (no json-glib) to hold the app's footprint to GTK3 + GLib/GIO + libcurl only. */
#ifndef LINKER_JSON_MIN_H
#define LINKER_JSON_MIN_H

#include <glib.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonValueType;

typedef struct JsonValue JsonValue;

struct JsonValue {
    JsonValueType type;
    union {
        gboolean b;
        gint64 n;
        gchar *s;
        GPtrArray *arr;  /* of JsonValue* */
        GPtrArray *obj;  /* of JsonMember* */
    } v;
};

typedef struct {
    gchar *key;
    JsonValue *value;
} JsonMember;

JsonValue *json_new_null(void);
JsonValue *json_new_bool(gboolean b);
JsonValue *json_new_number(gint64 n);
JsonValue *json_new_string(const gchar *s); /* NULL -> JSON_NULL */
JsonValue *json_new_array(void);
JsonValue *json_new_object(void);

void json_array_append(JsonValue *array, JsonValue *item);
void json_object_set(JsonValue *object, const gchar *key, JsonValue *value);

/* Returns NULL if key absent or object is not JSON_OBJECT. */
JsonValue *json_object_get(const JsonValue *object, const gchar *key);

/* Accessors with safe defaults when the value is missing/wrong type/JSON_NULL. */
const gchar *json_get_string(const JsonValue *object, const gchar *key, const gchar *default_value);
gint64 json_get_int(const JsonValue *object, const gchar *key, gint64 default_value);
gboolean json_get_bool(const JsonValue *object, const gchar *key, gboolean default_value);

void json_value_free(JsonValue *value);

/* Parses `text`; returns NULL and sets *error on failure. */
JsonValue *json_parse(const gchar *text, GError **error);

/* Serializes with 2-space pretty printing. Free with g_free. */
gchar *json_to_string(const JsonValue *value);

/* Serializes with no whitespace outside string values — one line, no embedded '\n' — for
 * newline-delimited wire protocols (dnsl's IPC socket). Free with g_free. */
gchar *json_to_string_compact(const JsonValue *value);

#endif
