/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Red Hat, Inc.
 * Copyright (C) 2017 Jan Alexander Steffens (heftig) <jan.steffens@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author:  Behdad Esfahbod, Red Hat, Inc.
 */

/* NOTE: This file is copied from gnome-settings-daemon, please keep it in sync */

#include "fc-monitor.h"

#include <gio/gio.h>
#include <fontconfig/fontconfig.h>

#define TIMEOUT_MILLISECONDS 1000

static void
fontconfig_cache_update_thread (GTask *task,
                                gpointer source_object G_GNUC_UNUSED,
                                gpointer task_data G_GNUC_UNUSED,
                                GCancellable *cancellable G_GNUC_UNUSED)
{
        if (FcConfigUptoDate (NULL)) {
                g_task_return_boolean (task, FALSE);
                return;
        }

        if (!FcInitReinitialize ()) {
                g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "FcInitReinitialize failed");
                return;
        }

        g_task_return_boolean (task, TRUE);
}

static void
fontconfig_cache_update_async (GAsyncReadyCallback callback,
                               gpointer user_data)
{
        GTask *task = g_task_new (NULL, NULL, callback, user_data);
        g_task_run_in_thread (task, fontconfig_cache_update_thread);
        g_object_unref (task);
}

static gboolean
fontconfig_cache_update_finish (GAsyncResult *result,
                                GError **error)
{
        return g_task_propagate_boolean (G_TASK (result), error);
}

typedef enum {
        UPDATE_IDLE,
        UPDATE_PENDING,
        UPDATE_RUNNING,
        UPDATE_RESTART,
} UpdateState;

struct _FcMonitor {
        GObject parent_instance;

        GPtrArray *monitors;

        guint timeout;
        UpdateState state;
        gboolean notify;
};

enum {
        SIGNAL_UPDATED,

        N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

static void fc_monitor_finalize (GObject *object);
static void monitor_files (FcMonitor *self, FcStrList *list);
static void stuff_changed (GFileMonitor *monitor, GFile *file, GFile *other_file,
                           GFileMonitorEvent event_type, gpointer data);
static void start_timeout (FcMonitor *self);
static gboolean start_update (gpointer data);
static void update_done (GObject *source_object, GAsyncResult *result, gpointer user_data);

G_DEFINE_TYPE (FcMonitor, fc_monitor, G_TYPE_OBJECT);

static void
fc_monitor_class_init (FcMonitorClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = fc_monitor_finalize;

        signals[SIGNAL_UPDATED] = g_signal_new ("updated",
                                                G_TYPE_FROM_CLASS (klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL,
                                                NULL,
                                                NULL,
                                                G_TYPE_NONE,
                                                0);
}

FcMonitor *
fc_monitor_new (void)
{
        return g_object_new (FC_TYPE_MONITOR, NULL);
}

static void
fc_monitor_init (FcMonitor *self G_GNUC_UNUSED)
{
        FcInit ();
}

static void
fc_monitor_finalize (GObject *object)
{
        FcMonitor *self = FC_MONITOR (object);

        if (self->timeout)
                g_source_remove (self->timeout);
        self->timeout = 0;

        g_clear_pointer (&self->monitors, g_ptr_array_unref);

        G_OBJECT_CLASS (fc_monitor_parent_class)->finalize (object);
}

void
fc_monitor_start (FcMonitor *self)
{
        g_return_if_fail (FC_IS_MONITOR (self));
        g_return_if_fail (self->monitors == NULL);

        self->monitors = g_ptr_array_new_with_free_func (g_object_unref);

        monitor_files (self, FcConfigGetConfigFiles (NULL));
        monitor_files (self, FcConfigGetFontDirs (NULL));
}

void
fc_monitor_stop (FcMonitor *self)
{
        g_return_if_fail (FC_IS_MONITOR (self));
        g_clear_pointer (&self->monitors, g_ptr_array_unref);
}

static void
monitor_files (FcMonitor *self,
               FcStrList *list)
{
        const char *str;

        while ((str = (const char *) FcStrListNext (list))) {
                GFile *file;
                GFileMonitor *monitor;

                file = g_file_new_for_path (str);

                g_debug ("Monitoring %s", str);
                monitor = g_file_monitor (file, G_FILE_MONITOR_NONE, NULL, NULL);

                g_object_unref (file);

                if (!monitor)
                        continue;

                g_signal_connect (monitor, "changed", G_CALLBACK (stuff_changed), self);

                g_ptr_array_add (self->monitors, monitor);
        }

        FcStrListDone (list);
}

static const gchar *
get_name (GType enum_type,
          gint enum_value)
{
        GEnumClass *klass = g_type_class_ref (enum_type);
        GEnumValue *value = g_enum_get_value (klass, enum_value);
        const gchar *name = value ? value->value_name : "(unknown)";
        g_type_class_unref (klass);
        return name;
}

static void
stuff_changed (GFileMonitor *monitor G_GNUC_UNUSED,
               GFile *file G_GNUC_UNUSED,
               GFile *other_file G_GNUC_UNUSED,
               GFileMonitorEvent event_type,
               gpointer data)
{
        FcMonitor *self = FC_MONITOR (data);
        const gchar *event_name = get_name (G_TYPE_FILE_MONITOR_EVENT, event_type);
        char *path = g_file_get_path (file);

        switch (self->state) {
        case UPDATE_IDLE:
                g_debug ("Got %-38s for %s: starting fontconfig update timeout", event_name, path);
                start_timeout (self);
                break;

        case UPDATE_PENDING:
                /* wait for quiescence */
                g_debug ("Got %-38s for %s: restarting fontconfig update timeout", event_name, path);
                g_source_remove (self->timeout);
                start_timeout (self);
                break;

        case UPDATE_RUNNING:
                g_debug ("Got %-38s for %s: restarting fontconfig update", event_name, path);
                self->state = UPDATE_RESTART;
                break;

        case UPDATE_RESTART:
                g_debug ("Got %-38s for %s: waiting on fontconfig update", event_name, path);
                break;
        }

	g_free (path);
}

static void
start_timeout (FcMonitor *self)
{
        self->state = UPDATE_PENDING;
        self->timeout = g_timeout_add (TIMEOUT_MILLISECONDS, start_update, self);
        g_source_set_name_by_id (self->timeout, "[gnome-settings-daemon] update");
}

static gboolean
start_update (gpointer data)
{
        FcMonitor *self = FC_MONITOR (data);

        self->state = UPDATE_RUNNING;
        self->timeout = 0;

        g_debug ("Timeout completed: starting fontconfig update");
        fontconfig_cache_update_async (update_done, g_object_ref (self));

        return G_SOURCE_REMOVE;
}

static void
update_done (GObject *source_object G_GNUC_UNUSED,
             GAsyncResult *result,
             gpointer data)
{
        FcMonitor *self = FC_MONITOR (data);
        gboolean restart = self->state == UPDATE_RESTART;
        GError *error = NULL;

        self->state = UPDATE_IDLE;

        if (fontconfig_cache_update_finish (result, &error)) {
                g_debug ("Fontconfig update successful");
                /* Remember we had a successful update even if we have to restart it */
                self->notify = TRUE;
        } else if (error) {
                g_warning ("Fontconfig update failed: %s", error->message);
                g_error_free (error);
        } else
                g_debug ("Fontconfig update was unnecessary");

        if (restart) {
                g_debug ("Concurrent change: restarting fontconfig update timeout");
                start_timeout (self);
        } else if (self->notify) {
                self->notify = FALSE;

                if (self->monitors) {
                        fc_monitor_stop (self);
                        fc_monitor_start (self);
                }

                /* we finish modifying self before emitting the signal,
                 * allowing the callback to stop us if it decides to. */
                g_signal_emit (self, signals[SIGNAL_UPDATED], 0);
        }

        /* release ref taken in start_update */
        g_object_unref (self);
}

#ifdef FONTCONFIG_MONITOR_TEST
static void
yay (void)
{
        g_message ("yay");
}

int
main (void)
{
        GMainLoop *loop = g_main_loop_new (NULL, TRUE);
        FcMonitor *monitor = fc_monitor_new ();

        fc_monitor_start (monitor);
        g_signal_connect (monitor, "updated", G_CALLBACK (yay), NULL);

        g_main_loop_run (loop);
        return 0;
}
#endif
