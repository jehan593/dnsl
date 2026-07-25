#include "ipc_protocol.h"
#include "json_min.h"
#include <string.h>

IpcCommand *ipc_command_new(IpcCommandType type)
{
    IpcCommand *cmd = g_new0(IpcCommand, 1);
    cmd->type = type;
    return cmd;
}

void ipc_command_free(IpcCommand *cmd)
{
    if (!cmd) return;
    g_free(cmd->provider_id);
    dns_provider_free(cmd->provider);
    g_free(cmd);
}

IpcStatus *ipc_status_new(void)
{
    IpcStatus *status = g_new0(IpcStatus, 1);
    status->providers = g_ptr_array_new_with_free_func((GDestroyNotify)dns_provider_free);
    return status;
}

void ipc_status_free(IpcStatus *status)
{
    if (!status) return;
    g_free(status->selected_provider_id);
    g_free(status->error_message);
    if (status->providers) g_ptr_array_free(status->providers, TRUE);
    g_free(status);
}

static const gchar *command_type_name(IpcCommandType type)
{
    switch (type) {
        case IPC_CMD_GET_STATUS: return "GetStatus";
        case IPC_CMD_ENABLE: return "Enable";
        case IPC_CMD_DISABLE: return "Disable";
        case IPC_CMD_SELECT_PROVIDER: return "SelectProvider";
        case IPC_CMD_ADD_CUSTOM_PROVIDER: return "AddCustomProvider";
        case IPC_CMD_REMOVE_CUSTOM_PROVIDER: return "RemoveCustomProvider";
    }
    return "GetStatus";
}

static gboolean command_type_from_name(const gchar *name, IpcCommandType *out)
{
    if (g_strcmp0(name, "GetStatus") == 0) { *out = IPC_CMD_GET_STATUS; return TRUE; }
    if (g_strcmp0(name, "Enable") == 0) { *out = IPC_CMD_ENABLE; return TRUE; }
    if (g_strcmp0(name, "Disable") == 0) { *out = IPC_CMD_DISABLE; return TRUE; }
    if (g_strcmp0(name, "SelectProvider") == 0) { *out = IPC_CMD_SELECT_PROVIDER; return TRUE; }
    if (g_strcmp0(name, "AddCustomProvider") == 0) { *out = IPC_CMD_ADD_CUSTOM_PROVIDER; return TRUE; }
    if (g_strcmp0(name, "RemoveCustomProvider") == 0) { *out = IPC_CMD_REMOVE_CUSTOM_PROVIDER; return TRUE; }
    return FALSE;
}

static JsonValue *provider_to_json(const DnsProvider *p)
{
    JsonValue *obj = json_new_object();
    json_object_set(obj, "id", json_new_string(p->id));
    json_object_set(obj, "name", json_new_string(p->name));
    json_object_set(obj, "tlsHost", json_new_string(p->tls_host));
    JsonValue *ips = json_new_array();
    for (guint i = 0; i < p->ip_count; i++) json_array_append(ips, json_new_string(p->ips[i]));
    json_object_set(obj, "ips", ips);
    json_object_set(obj, "port", json_new_number(p->port));
    json_object_set(obj, "isCustom", json_new_bool(p->is_custom));
    return obj;
}

static DnsProvider *provider_from_json(const JsonValue *obj)
{
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    const gchar *id = json_get_string(obj, "id", NULL);
    const gchar *name = json_get_string(obj, "name", NULL);
    const gchar *tls_host = json_get_string(obj, "tlsHost", NULL);
    if (!id || !name || !tls_host) return NULL;

    JsonValue *ips_val = json_object_get(obj, "ips");
    GPtrArray *ips = g_ptr_array_new();
    if (ips_val && ips_val->type == JSON_ARRAY) {
        for (guint i = 0; i < ips_val->v.arr->len; i++) {
            JsonValue *item = g_ptr_array_index(ips_val->v.arr, i);
            if (item->type == JSON_STRING) g_ptr_array_add(ips, item->v.s);
        }
    }
    gint64 port = json_get_int(obj, "port", 853);
    gboolean is_custom = json_get_bool(obj, "isCustom", TRUE);
    DnsProvider *p = dns_provider_new(id, name, tls_host,
                                       (const gchar *const *)ips->pdata, ips->len, (int)port, is_custom);
    g_ptr_array_free(ips, TRUE);
    return p;
}

gchar *ipc_serialize_command(const IpcCommand *cmd)
{
    JsonValue *obj = json_new_object();
    json_object_set(obj, "type", json_new_string(command_type_name(cmd->type)));
    json_object_set(obj, "providerId", json_new_string(cmd->provider_id));
    json_object_set(obj, "provider", cmd->provider ? provider_to_json(cmd->provider) : json_new_null());
    gchar *text = json_to_string_compact(obj);
    json_value_free(obj);
    return text;
}

gchar *ipc_serialize_status(const IpcStatus *status)
{
    JsonValue *obj = json_new_object();
    json_object_set(obj, "enabled", json_new_bool(status->enabled));
    json_object_set(obj, "selectedProviderId", json_new_string(status->selected_provider_id));
    JsonValue *providers = json_new_array();
    for (guint i = 0; i < status->providers->len; i++)
        json_array_append(providers, provider_to_json(g_ptr_array_index(status->providers, i)));
    json_object_set(obj, "providers", providers);
    json_object_set(obj, "errorMessage", json_new_string(status->error_message));
    gchar *text = json_to_string_compact(obj);
    json_value_free(obj);
    return text;
}

IpcCommand *ipc_parse_command(const gchar *line)
{
    GError *error = NULL;
    JsonValue *root = json_parse(line, &error);
    if (!root) { g_clear_error(&error); return NULL; }

    IpcCommandType type;
    if (!command_type_from_name(json_get_string(root, "type", NULL), &type)) {
        json_value_free(root);
        return NULL;
    }

    IpcCommand *cmd = ipc_command_new(type);
    const gchar *provider_id = json_get_string(root, "providerId", NULL);
    if (provider_id) cmd->provider_id = g_strdup(provider_id);
    cmd->provider = provider_from_json(json_object_get(root, "provider"));

    json_value_free(root);
    return cmd;
}

IpcStatus *ipc_parse_status(const gchar *line)
{
    GError *error = NULL;
    JsonValue *root = json_parse(line, &error);
    if (!root) { g_clear_error(&error); return NULL; }

    IpcStatus *status = ipc_status_new();
    status->enabled = json_get_bool(root, "enabled", FALSE);
    const gchar *selected = json_get_string(root, "selectedProviderId", NULL);
    if (selected) status->selected_provider_id = g_strdup(selected);
    const gchar *error_message = json_get_string(root, "errorMessage", NULL);
    if (error_message) status->error_message = g_strdup(error_message);

    JsonValue *providers = json_object_get(root, "providers");
    if (providers && providers->type == JSON_ARRAY) {
        for (guint i = 0; i < providers->v.arr->len; i++) {
            DnsProvider *p = provider_from_json(g_ptr_array_index(providers->v.arr, i));
            if (p) g_ptr_array_add(status->providers, p);
        }
    }

    json_value_free(root);
    return status;
}
