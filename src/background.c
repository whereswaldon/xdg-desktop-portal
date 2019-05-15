/*
 * Copyright © 2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "background.h"
#include "request.h"
#include "permissions.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"
#include "flatpak-instance.h"

#define PERMISSION_TABLE "background"
#define PERMISSION_ID "background"

typedef struct _Background Background;
typedef struct _BackgroundClass BackgroundClass;

struct _Background
{
  XdpBackgroundSkeleton parent_instance;
};

struct _BackgroundClass
{
  XdpBackgroundSkeletonClass parent_class;
};

static XdpImplAccess *access_impl;
static XdpImplBackground *background_impl;
static Background *background;

GType background_get_type (void) G_GNUC_CONST;
static void background_iface_init (XdpBackgroundIface *iface);

G_DEFINE_TYPE_WITH_CODE (Background, background, XDP_TYPE_BACKGROUND_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_BACKGROUND, background_iface_init));


typedef enum { UNSET, NO, YES, ASK } Permission;

static GVariant *
get_all_permissions (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;

  if (!xdp_impl_permission_store_call_lookup_sync (get_permission_store (),
                                                   PERMISSION_TABLE,
                                                   PERMISSION_ID,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No background permissions found: %s", error->message);
      return NULL;
    }

  return g_steal_pointer (&out_perms);
}

static Permission
get_one_permission (const char *app_id,
                    GVariant   *perms)
{
  const char **permissions;

  if (perms == NULL)
    {
      g_debug ("No background permissions found");

      return UNSET;
    }
  else if (!g_variant_lookup (perms, app_id, "^a&s", &permissions))
    {
      g_debug ("No background permissions stored for: app %s", app_id);

      return UNSET;
    }
  else if (g_strv_length ((char **)permissions) != 1)
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong background permission format, ignoring (%s)", a);
      return UNSET;
    }

  g_debug ("permission store: background, app %s -> %s", app_id, permissions[0]);

  if (strcmp (permissions[0], "yes") == 0)
    return YES;
  else if (strcmp (permissions[0], "no") == 0)
    return NO;
  else if (strcmp (permissions[0], "ask") == 0)
    return ASK;
  else
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
    }

  return UNSET;
}

static Permission
get_permission (const char *app_id)
{
  g_autoptr(GVariant) perms = NULL;

  perms = get_all_permissions ();
  if (perms)
    return get_one_permission (app_id, perms);

  return UNSET;
}

static void
set_permission (const char *app_id,
                Permission permission)
{
  g_autoptr(GError) error = NULL;
  const char *permissions[2];

  if (permission == ASK)
    permissions[0] = "ask";
  else if (permission == YES)
    permissions[0] = "yes";
  else if (permission == NO)
    permissions[0] = "no";
  else
    {
      g_warning ("Wrong permission format, ignoring");
      return;
    }
  permissions[1] = NULL;

  if (!xdp_impl_permission_store_call_set_permission_sync (get_permission_store (),
                                                           PERMISSION_TABLE,
                                                           TRUE,
                                                           PERMISSION_ID,
                                                           app_id,
                                                           (const char * const*)permissions,
                                                           NULL,
                                                           &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error updating permission store: %s", error->message);
    }
}

typedef enum {
  AUTOSTART_FLAGS_NONE        = 0,
  AUTOSTART_FLAGS_ACTIVATABLE = 1 << 0,
} AutostartFlags;

static void
handle_request_background_in_thread_func (GTask *task,
                                          gpointer source_object,
                                          gpointer task_data,
                                          GCancellable *cancellable)
{
  Request *request = (Request *)task_data;
  GVariant *options;
  const char *app_id;
  Permission permission;
  const char *reason = NULL;
  gboolean autostart_requested = FALSE;
  gboolean autostart_enabled;
  gboolean allowed;
  g_autoptr(GError) error = NULL;
  const char * const *autostart_exec = { NULL };
  AutostartFlags autostart_flags = AUTOSTART_FLAGS_NONE;
  gboolean activatable = FALSE;
  g_auto(GStrv) commandline = NULL;

  REQUEST_AUTOLOCK (request);

  options = (GVariant *)g_object_get_data (G_OBJECT (request), "options");
  g_variant_lookup (options, "reason", "&s", &reason);
  g_variant_lookup (options, "autostart", "b", &autostart_requested);
  g_variant_lookup (options, "commandline", "^a&s", &autostart_exec);
  g_variant_lookup (options, "dbus-activatable", "b", &activatable);

  if (activatable)
    autostart_flags |= AUTOSTART_FLAGS_ACTIVATABLE;

  app_id = xdp_app_info_get_id (request->app_info);

  if (xdp_app_info_is_host (request->app_info))
    permission = YES;
  else
    permission = get_permission (app_id);

  g_debug ("Handle RequestBackground for %s\n", app_id);

  if (permission == ASK || permission == UNSET)
    {
      GVariantBuilder opt_builder;
      g_autofree char *title = NULL;
      g_autofree char *subtitle = NULL;
      g_autofree char *body = NULL;
      guint32 response = 2;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;
      g_autoptr(GAppInfo) info = NULL;

      info = xdp_app_info_load_app_info (request->app_info);

      title = g_strdup_printf (_("Allow %s to run in the background?"), info ? g_app_info_get_display_name (info) : app_id);
      if (reason)
        subtitle = g_strdup (reason);
      else if (autostart_requested)
        subtitle = g_strdup_printf (_("%s requests to be started automatically and run in the background."), info ? g_app_info_get_display_name (info) : app_id);
      else
        subtitle = g_strdup_printf (_("%s requests to run in the background."), info ? g_app_info_get_display_name (info) : app_id);
      body = g_strdup (_("The ‘run in background’ permission can be changed at any time from the application settings."));

      g_debug ("Calling backend for background access for: %s", app_id);

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&opt_builder, "{sv}", "deny_label", g_variant_new_string (_("Don't allow")));
      g_variant_builder_add (&opt_builder, "{sv}", "grant_label", g_variant_new_string (_("Allow")));
      if (!xdp_impl_access_call_access_dialog_sync (access_impl,
                                                    request->id,
                                                    app_id,
                                                    "",
                                                    title,
                                                    subtitle,
                                                    body,
                                                    g_variant_builder_end (&opt_builder),
                                                    &response,
                                                    &results,
                                                    NULL,
                                                    &error))
        {
          g_warning ("AccessDialog call failed: %s", error->message);
          g_clear_error (&error);
        }

      allowed = response == 0;

      if (permission == UNSET)
        set_permission (app_id, allowed ? YES : NO);
    }
  else
    allowed = permission == YES ? TRUE : FALSE;

  g_debug ("Setting autostart for %s to %s", app_id,
           allowed && autostart_requested ? "enabled" : "disabled");

  commandline = xdp_app_info_rewrite_commandline (request->app_info, autostart_exec);
  if (!xdp_impl_background_call_enable_autostart_sync (background_impl,
                                                       app_id,
                                                       allowed && autostart_requested,
                                                       (const char * const *)commandline,
                                                       autostart_flags,
                                                       &autostart_enabled,
                                                       NULL,
                                                       &error))
    {
      g_warning ("EnableAutostart call failed: %s", error->message);
      g_clear_error (&error);
    }

  if (request->exported)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&results, "{sv}", "background", g_variant_new_boolean (allowed));
      g_variant_builder_add (&results, "{sv}", "autostart", g_variant_new_boolean (autostart_enabled));
      xdp_request_emit_response (XDP_REQUEST (request),
                                 allowed ? XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS : XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static gboolean
validate_reason (const char *key,
                 GVariant *value,
                 GVariant *options,
                 GError **error)
{
  const char *string = g_variant_get_string (value, NULL);

  if (g_utf8_strlen (string, -1) > 256)
    {
      g_set_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                   "Not accepting overly long reasons");
      return FALSE;
    }

  return TRUE;
}

static XdpOptionKey background_options[] = {
  { "reason", G_VARIANT_TYPE_STRING, validate_reason },
  { "autostart", G_VARIANT_TYPE_BOOLEAN, NULL },
  { "commandline", G_VARIANT_TYPE_STRING_ARRAY, NULL },
  { "dbus-activatable", G_VARIANT_TYPE_BOOLEAN, NULL },
};

static gboolean
handle_request_background (XdpBackground *object,
                           GDBusMethodInvocation *invocation,
                           const char *arg_window,
                           GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  g_autoptr(GTask) task = NULL;
  GVariantBuilder opt_builder;
  g_autoptr(GVariant) options = NULL;

  REQUEST_AUTOLOCK (request);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_filter_options (arg_options, &opt_builder,
                      background_options, G_N_ELEMENTS (background_options),
                      NULL);

  options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));

  g_object_set_data_full (G_OBJECT (request), "window", g_strdup (arg_window), g_free);
  g_object_set_data_full (G_OBJECT (request), "options", g_variant_ref (options), (GDestroyNotify)g_variant_unref);

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (access_impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (access_impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  request_set_impl_request (request, impl_request);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_background_complete_request_background (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_request_background_in_thread_func);

  return TRUE;
}

static void
background_iface_init (XdpBackgroundIface *iface)
{
  iface->handle_request_background = handle_request_background;
}

static void
background_init (Background *background)
{
  xdp_background_set_version (XDP_BACKGROUND (background), 1);
}

static void
background_class_init (BackgroundClass *klass)
{
}

/* background monitor */

typedef enum { BACKGROUND, RUNNING, ACTIVE } AppState;

static GHashTable *
get_app_states (void)
{
  g_autoptr(GVariant) apps = NULL;
  g_autoptr(GHashTable) app_states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const char *appid;
  GVariant *value;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_background_call_get_app_state_sync (background_impl, &apps, NULL, &error))
    {
      g_warning ("Failed to get application states: %s", error->message);
      return NULL;
    }

  g_autoptr(GVariantIter) iter = g_variant_iter_new (apps);
  while (g_variant_iter_loop (iter, "{&sv}", &appid, &value))
    {
      AppState state = g_variant_get_uint32 (value);
      g_hash_table_insert (app_states, g_strdup (appid), GINT_TO_POINTER (state));
    }

  return g_steal_pointer (&app_states);
}

static AppState
get_one_app_state (const char *app_id,
                   GHashTable *app_states)
{
  return (AppState)GPOINTER_TO_INT (g_hash_table_lookup (app_states, app_id));
}

typedef struct {
  FlatpakInstance *instance;
  int stamp;
  AppState state;
  char *handle;
  gboolean notified;
  Permission permission;
} InstanceData;

static void
instance_data_free (gpointer data)
{
  InstanceData *idata = data;

  g_object_unref (idata->instance);
  g_free (idata->handle);

  g_free (idata);
}

/* Only used by the monitor thread, so no locking needed
 * instance ID -> InstanceData
 */
static GHashTable *applications;

static void
close_notification (const char *handle)
{
  g_dbus_connection_call (g_dbus_proxy_get_connection (G_DBUS_PROXY (background)),
                          g_dbus_proxy_get_name (G_DBUS_PROXY (background_impl)),
                          handle,
                          "org.freedesktop.impl.portal.Request",
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, NULL, NULL);
}

static void
remove_outdated_instances (int stamp)
{
  GHashTableIter iter;
  char *id;
  InstanceData *data;

  g_hash_table_iter_init (&iter, applications);
  while (g_hash_table_iter_next (&iter, (gpointer *)&id, (gpointer *)&data))
    {
      if (data->stamp < stamp)
        {
          if (data->handle)
            close_notification (data->handle);
          g_hash_table_iter_remove (&iter);
        }
    }
}

static char *
flatpak_instance_get_display_name (FlatpakInstance *instance)
{
  const char *app_id = flatpak_instance_get_app (instance);
  if (app_id[0] != 0)
    {
      g_autofree char *desktop_id = NULL;
      g_autoptr(GAppInfo) info = NULL;

      desktop_id = g_strconcat (app_id, ".desktop", NULL);
      info = (GAppInfo*)g_desktop_app_info_new (desktop_id);

      if (info)
        return g_strdup (g_app_info_get_display_name (info));
    }

  return g_strdup (app_id);
}

static void
kill_instance (const char *id)
{
  InstanceData *idata;

  idata = g_hash_table_lookup (applications, id);

  if (idata)
    {
      g_debug ("Killing app %s", flatpak_instance_get_app (idata->instance));
      kill (flatpak_instance_get_child_pid (idata->instance), SIGKILL);
    }
}

typedef struct {
  char *app_id;
  char *id;
  Permission perm;
} DoneData;

static void
done_data_free (gpointer data)
{
  DoneData *ddata = data;

  g_free (ddata->app_id);
  g_free (ddata->id);
  g_free (ddata);
}

typedef enum {
  FORBID = 0,
  ALLOW  = 1
} NotifyResult;

static void
notify_background_done (GObject *source,
                        GAsyncResult *res,
                        gpointer data)
{
  DoneData *ddata = (DoneData *)data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) results = NULL;
  guint response;
  guint result;
  InstanceData *idata;

  if (!xdp_impl_background_call_notify_background_finish (background_impl,
                                                          &response,
                                                          &results,
                                                          res,
                                                          &error))
    {
      g_warning ("Error from background backend: %s", error->message);
      done_data_free (ddata);
      return;
    }

  g_variant_lookup (results, "result", "u", &result);

  if (result == ALLOW)
    {
      g_debug ("Allowing app %s to run in background", ddata->app_id);

      if (ddata->perm != ASK)
        ddata->perm = YES;
    }
  else if (result == FORBID)
    {
      g_debug ("Forbid app %s to run in background", ddata->app_id);

      if (ddata->perm != ASK)
        ddata->perm = NO;

      kill_instance (ddata->id);
    }
  else
    g_debug ("Unexpected response from NotifyBackground: %u", result);

  set_permission (ddata->app_id, ddata->perm);

  idata = g_hash_table_lookup (applications, ddata->id);
  if (idata)
    {
      g_clear_pointer (&idata->handle, g_free);
      idata->permission = ddata->perm;
    }

  done_data_free (ddata);
}

static void
send_notification (InstanceData *idata)
{
  FlatpakInstance *instance = idata->instance;
  DoneData *ddata;
  g_autofree char *name = flatpak_instance_get_display_name (instance);
  static int count;
  char *handle;

  ddata = g_new (DoneData, 1);
  ddata->app_id = g_strdup (flatpak_instance_get_app (instance));
  ddata->id = g_strdup (flatpak_instance_get_id (instance));
  ddata->perm = idata->permission;

  g_debug ("Notify background for %s", ddata->app_id);

  handle = g_strdup_printf ("/org/freedesktop/portal/desktop/notify/background%d", count++);

  xdp_impl_background_call_notify_background (background_impl,
                                              handle,
                                              ddata->app_id,
                                              name,
                                              NULL,
                                              notify_background_done,
                                              ddata);

  g_assert (idata->handle == NULL);
  idata->handle = handle;
  idata->notified = TRUE;
}

static void
check_background_apps (void)
{
  g_autoptr(GVariant) perms = NULL;
  g_autoptr(GHashTable) app_states = NULL;
  g_autoptr(GPtrArray) instances = NULL;
  int i;
  static int stamp;

  app_states = get_app_states ();
  if (app_states == NULL)
    return;

  g_debug ("Checking background permissions");

  perms = get_all_permissions ();
  instances = flatpak_instance_get_all ();

  stamp++;

  for (i = 0; i < instances->len; i++)
    {
      FlatpakInstance *instance = g_ptr_array_index (instances, i);
      const char *id;
      const char *app_id;
      InstanceData *idata;
      Permission permission;
      const char *state_names[] = { "background", "running", "active" };
      gboolean is_new = FALSE;

      if (!flatpak_instance_is_running (instance))
        continue;

      id = flatpak_instance_get_id (instance);
      app_id = flatpak_instance_get_app (instance);
      idata = g_hash_table_lookup (applications, id);

      if (!idata)
        {
          is_new = TRUE;
          idata = g_new0 (InstanceData, 1);
          idata->instance = g_object_ref (instance);
          g_hash_table_insert (applications, g_strdup (id), idata);
        }

      idata->stamp = stamp;
      idata->state = get_one_app_state (app_id, app_states);

      g_debug ("App %s is %s", app_id, state_names[idata->state]);

      permission = get_one_permission (app_id, perms);

      if (idata->permission != permission)
        {
          /* Notify again if permissions change */
          idata->permission = permission;
          idata->notified = FALSE;
        }

      /* If the app is not in the list yet, add it,
       * but don't notify yet - this gives apps some
       * leeway to get their window app. If it is still
       * in the background next time around, we'll proceed
       * to the next step.
       */
      if (idata->state != BACKGROUND || idata->notified || is_new)
        continue;

      switch (idata->permission)
        {
        case NO:
          kill_instance (id);
          idata->stamp = 0;
          break;

        case ASK:
        case UNSET:
          send_notification (idata);
          break;

        case YES:
        default:
          break;
        }
    }

  remove_outdated_instances (stamp);
}

static gpointer
background_monitor (gpointer data)
{
  applications = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, instance_data_free);
  while (1)
    {
      check_background_apps ();
      sleep (20);
    }

  g_clear_pointer (&applications, g_hash_table_unref);

  return NULL;
}

static void
start_background_monitor (void)
{
  g_autoptr(GThread) thread = NULL;

  g_debug ("Starting background app monitor");

  thread = g_thread_new ("background monitor", background_monitor, NULL);
}

GDBusInterfaceSkeleton *
background_create (GDBusConnection *connection,
                   const char *dbus_name_access,
                   const char *dbus_name_background)
{
  g_autoptr(GError) error = NULL;

  access_impl = xdp_impl_access_proxy_new_sync (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                dbus_name_access,
                                                DESKTOP_PORTAL_OBJECT_PATH,
                                                NULL,
                                                &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (access_impl), G_MAXINT);

  background_impl = xdp_impl_background_proxy_new_sync (connection,
                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                        dbus_name_background,
                                                        DESKTOP_PORTAL_OBJECT_PATH,
                                                        NULL,
                                                        &error);
  if (background_impl == NULL)
    {
      g_warning ("Failed to create background proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (background_impl), G_MAXINT);
  background = g_object_new (background_get_type (), NULL);

  start_background_monitor ();

  return G_DBUS_INTERFACE_SKELETON (background);
}
