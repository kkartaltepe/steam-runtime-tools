/*
 * pressure-vessel-launcher — accept IPC requests to create child processes
 *
 * Copyright © 2018 Red Hat, Inc.
 * Copyright © 2020 Collabora Ltd.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Based on xdg-desktop-portal, flatpak-portal and flatpak-spawn.
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <sysexits.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "steam-runtime-tools/glib-backports-internal.h"
#include "steam-runtime-tools/log-internal.h"
#include "steam-runtime-tools/utils-internal.h"
#include "libglnx/libglnx.h"

#include "flatpak-utils-base-private.h"
#include "launcher.h"
#include "portal-listener.h"
#include "utils.h"

/* Absence of GConnectFlags; slightly more readable than a magic number */
#define CONNECT_FLAGS_NONE (0)

typedef struct
{
  GObject parent;
  PvPortalListener *listener;
  GHashTable *client_pid_data_hash;
  GMainLoop *main_loop;
  PvLauncher1 *launcher;
  guint exit_on_readable_id;
  guint signals_id;
  int exit_status;
} PvLauncherServer;

typedef struct
{
  GObjectClass parent;
} PvLauncherServerClass;

#define PV_TYPE_LAUNCHER_SERVER (pv_launcher_server_get_type ())
#define PV_LAUNCHER_SERVER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_LAUNCHER_SERVER, PvLauncherServer))
#define PV_LAUNCHER_SERVER_CLASS(cls) (G_TYPE_CHECK_CLASS_CAST ((cls), PV_TYPE_LAUNCHER_SERVER, PvLauncherServerClass))
#define PV_IS_LAUNCHER_SERVER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_LAUNCHER_SERVER))
#define PV_IS_LAUNCHER_SERVER_CLASS(cls) (G_TYPE_CHECK_CLASS_TYPE ((cls), PV_TYPE_LAUNCHER_SERVER))
#define PV_LAUNCHER_SERVER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), PV_TYPE_LAUNCHER_SERVER, PvLauncherServerClass)
GType pv_launcher_server_get_type (void);

G_DEFINE_TYPE (PvLauncherServer, pv_launcher_server, G_TYPE_OBJECT)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PvLauncherServer, g_object_unref)

static void
pv_launcher_server_init (PvLauncherServer *self)
{
  g_return_if_fail (PV_IS_LAUNCHER_SERVER (self));
  self->exit_status = -1;
}

static void
pv_launcher_server_stop (PvLauncherServer *self)
{
  if (self->exit_on_readable_id > 0)
    {
      g_source_remove (self->exit_on_readable_id);
      self->exit_on_readable_id = 0;
    }

  if (self->signals_id > 0)
    {
      g_source_remove (self->signals_id);
      self->signals_id = 0;
    }
}

static void
pv_launcher_server_dispose (GObject *object)
{
  PvLauncherServer *self = PV_LAUNCHER_SERVER (object);

  pv_launcher_server_stop (self);
  g_clear_object (&self->listener);
  g_clear_pointer (&self->client_pid_data_hash, g_hash_table_unref);
  g_clear_pointer (&self->main_loop, g_main_loop_unref);
  g_clear_object (&self->launcher);

  G_OBJECT_CLASS (pv_launcher_server_parent_class)->dispose (object);
}

static void
pv_launcher_server_class_init (PvLauncherServerClass *cls)
{
  GObjectClass *object_class = G_OBJECT_CLASS (cls);

  object_class->dispose = pv_launcher_server_dispose;
}

static void
skeleton_died_cb (gpointer data)
{
  PvLauncherServer *self = data;

  g_return_if_fail (PV_IS_LAUNCHER_SERVER (self));
  g_debug ("skeleton finalized, exiting");

  if (self->main_loop != NULL)
    g_main_loop_quit (self->main_loop);

  /* paired with ref in pv_launcher_server_export */
  g_object_unref (self);
}

static gboolean
unref_skeleton_in_timeout_cb (gpointer user_data)
{
  PvLauncherServer *self = user_data;

  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (self), G_SOURCE_REMOVE);
  g_clear_object (&self->launcher);
  return G_SOURCE_REMOVE;
}

static void
pv_launcher_server_unref_skeleton_in_timeout (PvLauncherServer *self)
{
  g_return_if_fail (PV_IS_LAUNCHER_SERVER (self));

  if (self->listener != NULL)
    pv_portal_listener_release_name (self->listener);

  /* After we've lost the name we drop the main ref on the helper
     so that we'll exit when it drops to zero. However, if there are
     outstanding calls these will keep the refcount up during the
     execution of them. We do the unref on a timeout to make sure
     we're completely draining the queue of (stale) requests. */
  g_timeout_add_full (G_PRIORITY_DEFAULT, 500, unref_skeleton_in_timeout_cb,
                      g_object_ref (self), g_object_unref);
}

typedef struct
{
  PvLauncherServer *server;   /* (unowned) to avoid circular ref */
  GDBusConnection *connection;
  GPid pid;
  gchar *client;
  guint child_watch;
  gboolean terminate_after;
} PidData;

static void
pid_data_free (PidData *data)
{
  g_clear_object (&data->connection);
  g_free (data->client);
  g_free (data);
}

static void
pv_launcher_server_terminate_children (PvLauncherServer *self,
                                       int signum)
{
  GHashTableIter iter;
  PidData *pid_data = NULL;
  gpointer value = NULL;

  /* pass the signal on to each process group led by one of our
   * child processes */
  g_hash_table_iter_init (&iter, self->client_pid_data_hash);

  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      pid_data = value;
      killpg (pid_data->pid, signum);
    }
}

static void
child_watch_died (GPid     pid,
                  gint     status,
                  gpointer user_data)
{
  PidData *pid_data = user_data;
  g_autoptr(PvLauncherServer) self = g_object_ref (pid_data->server);
  g_autoptr(GVariant) signal_variant = NULL;
  gboolean terminate_after = pid_data->terminate_after;

  g_debug ("Child %d died: wait status %d", pid_data->pid, status);

  signal_variant = g_variant_ref_sink (g_variant_new ("(uu)", pid, status));
  g_dbus_connection_emit_signal (pid_data->connection,
                                 pid_data->client,
                                 LAUNCHER_PATH,
                                 LAUNCHER_IFACE,
                                 "ProcessExited",
                                 signal_variant,
                                 NULL);

  /* This frees the pid_data, so be careful */
  g_hash_table_remove (self->client_pid_data_hash, GUINT_TO_POINTER (pid_data->pid));

  if (terminate_after)
    {
      g_debug ("Main pid %d died, terminating...", pid);
      pv_launcher_server_terminate_children (self, SIGTERM);
      pv_launcher_server_unref_skeleton_in_timeout (self);
    }
}

typedef struct
{
  int from;
  int to;
  int final;
} FdMapEntry;

typedef struct
{
  FdMapEntry *fd_map;
  int         fd_map_len;
} ChildSetupData;

static void
drop_cloexec (int fd)
{
  fcntl (fd, F_SETFD, 0);
}

static void
child_setup_func (gpointer user_data)
{
  ChildSetupData *data = (ChildSetupData *) user_data;
  FdMapEntry *fd_map = data->fd_map;
  sigset_t set;
  int i;

  flatpak_close_fds_workaround (3);

  /* Unblock all signals */
  sigemptyset (&set);
  if (pthread_sigmask (SIG_SETMASK, &set, NULL) == -1)
    _srt_async_signal_safe_error ("Failed to unblock signals when starting child\n",
                                  LAUNCH_EX_FAILED);

  /* Reset the handlers for all signals to their defaults. */
  for (i = 1; i < NSIG; i++)
    {
      if (i != SIGSTOP && i != SIGKILL)
        signal (i, SIG_DFL);
    }

  for (i = 0; i < data->fd_map_len; i++)
    {
      if (fd_map[i].from != fd_map[i].to)
        {
          dup2 (fd_map[i].from, fd_map[i].to);
          close (fd_map[i].from);
        }
    }

  /* Second pass in case we needed an in-between fd value to avoid conflicts */
  for (i = 0; i < data->fd_map_len; i++)
    {
      if (fd_map[i].to != fd_map[i].final)
        {
          dup2 (fd_map[i].to, fd_map[i].final);
          close (fd_map[i].to);
        }

      /* Ensure we inherit the final fd value */
      drop_cloexec (fd_map[i].final);
    }

  /* We become our own session and process group, because it never makes sense
     to share the flatpak-session-helper dbus activated process group */
  setsid ();
  setpgid (0, 0);
}

static gboolean
handle_launch (PvLauncher1           *object,
               GDBusMethodInvocation *invocation,
               GUnixFDList           *fd_list,
               const gchar           *arg_cwd_path,
               const gchar *const    *arg_argv,
               GVariant              *arg_fds,
               GVariant              *arg_envs,
               guint                  arg_flags,
               GVariant              *arg_options,
               PvLauncherServer      *self)
{
  g_autoptr(GError) error = NULL;
  ChildSetupData child_setup_data = { NULL };
  GPid pid;
  PidData *pid_data;
  gsize i, j, n_fds, n_envs;
  const gint *fds = NULL;
  gint fds_len = 0;
  g_autofree FdMapEntry *fd_map = NULL;
  g_auto(GStrv) env = NULL;
  g_auto(GStrv) unset_env = NULL;
  gint32 max_fd;
  gboolean terminate_after = FALSE;

  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (self),
                        G_DBUS_METHOD_INVOCATION_UNHANDLED);

  if (fd_list != NULL)
    fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);

  if (*arg_cwd_path == 0)
    arg_cwd_path = NULL;

  if (arg_argv == NULL || *arg_argv == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "No command given");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if ((arg_flags & ~PV_LAUNCH_FLAGS_MASK) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", arg_flags & ~PV_LAUNCH_FLAGS_MASK);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_lookup (arg_options, "terminate-after", "b", &terminate_after);

  g_info ("Running spawn command %s", arg_argv[0]);

  n_fds = 0;
  if (fds != NULL)
    n_fds = g_variant_n_children (arg_fds);
  fd_map = g_new0 (FdMapEntry, n_fds);

  child_setup_data.fd_map = fd_map;
  child_setup_data.fd_map_len = n_fds;

  max_fd = -1;
  for (i = 0; i < n_fds; i++)
    {
      gint32 handle, dest_fd;
      int handle_fd;

      g_variant_get_child (arg_fds, i, "{uh}", &dest_fd, &handle);
      if (handle >= fds_len)
        continue;
      handle_fd = fds[handle];

      fd_map[i].to = dest_fd;
      fd_map[i].from = handle_fd;
      fd_map[i].final = fd_map[i].to;

      max_fd = MAX (max_fd, fd_map[i].to);
      max_fd = MAX (max_fd, fd_map[i].from);
    }

  /* We make a second pass over the fds to find if any "to" fd index
     overlaps an already in use fd (i.e. one in the "from" category
     that are allocated randomly). If a fd overlaps "to" fd then its
     a caller issue and not our fault, so we ignore that. */
  for (i = 0; i < n_fds; i++)
    {
      int to_fd = fd_map[i].to;
      gboolean conflict = FALSE;

      /* At this point we're fine with using "from" values for this
         value (because we handle to==from in the code), or values
         that are before "i" in the fd_map (because those will be
         closed at this point when dup:ing). However, we can't
         reuse a fd that is in "from" for j > i. */
      for (j = i + 1; j < n_fds; j++)
        {
          int from_fd = fd_map[j].from;
          if (from_fd == to_fd)
            {
              conflict = TRUE;
              break;
            }
        }

      if (conflict)
        fd_map[i].to = ++max_fd;
    }

  if (arg_flags & PV_LAUNCH_FLAGS_CLEAR_ENV)
    {
      char *empty[] = { NULL };

      env = g_strdupv (empty);
    }
  else
    {
      env = g_strdupv (self->listener->original_environ);
    }

  n_envs = g_variant_n_children (arg_envs);
  for (i = 0; i < n_envs; i++)
    {
      const char *var = NULL;
      const char *val = NULL;
      g_variant_get_child (arg_envs, i, "{&s&s}", &var, &val);

      /* Ignore PWD: we special-case that later */
      if (g_strcmp0 (var, "PWD") == 0)
        continue;

      env = g_environ_setenv (env, var, val, TRUE);
    }

  g_variant_lookup (arg_options, "unset-env", "^as", &unset_env);

  for (i = 0; unset_env != NULL && unset_env[i] != NULL; i++)
    {
      /* Again ignore PWD */
      if (g_strcmp0 (unset_env[i], "PWD") == 0)
        continue;

      g_debug ("Unsetting the environment variable %s...", unset_env[i]);
      env = g_environ_unsetenv (env, unset_env[i]);
    }

  if (arg_cwd_path == NULL)
    env = g_environ_setenv (env, "PWD", self->listener->original_cwd_l,
                            TRUE);
  else
    env = g_environ_setenv (env, "PWD", arg_cwd_path, TRUE);

  /* We use LEAVE_DESCRIPTORS_OPEN to work around dead-lock, see flatpak_close_fds_workaround */
  if (!g_spawn_async_with_pipes (arg_cwd_path,
                                 (gchar **) arg_argv,
                                 env,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                                 child_setup_func, &child_setup_data,
                                 &pid,
                                 NULL,
                                 NULL,
                                 NULL,
                                 &error))
    {
      gint code = G_DBUS_ERROR_FAILED;

      if (g_error_matches (error, G_SPAWN_ERROR, G_SPAWN_ERROR_ACCES))
        code = G_DBUS_ERROR_ACCESS_DENIED;
      else if (g_error_matches (error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        code = G_DBUS_ERROR_FILE_NOT_FOUND;

      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, code,
                                             "Failed to start command: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  pid_data = g_new0 (PidData, 1);
  pid_data->server = self;
  pid_data->connection = g_object_ref (g_dbus_method_invocation_get_connection (invocation));
  pid_data->pid = pid;
  pid_data->client = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  pid_data->terminate_after = terminate_after;
  pid_data->child_watch = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                  pid,
                                                  child_watch_died,
                                                  pid_data,
                                                  NULL);

  g_debug ("Client Pid is %d", pid_data->pid);

  g_hash_table_replace (self->client_pid_data_hash,
                        GUINT_TO_POINTER (pid_data->pid),
                        pid_data);

  pv_launcher1_complete_launch (object, invocation, NULL, pid);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_send_signal (PvLauncher1           *object,
                    GDBusMethodInvocation *invocation,
                    guint                  arg_pid,
                    guint                  arg_signal,
                    gboolean               arg_to_process_group,
                    PvLauncherServer      *self)
{
  PidData *pid_data = NULL;

  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (self),
                        G_DBUS_METHOD_INVOCATION_UNHANDLED);
  g_debug ("SendSignal(%d, %d)", arg_pid, arg_signal);

  pid_data = g_hash_table_lookup (self->client_pid_data_hash,
                                  GUINT_TO_POINTER (arg_pid));
  if (pid_data == NULL ||
      pid_data->connection != g_dbus_method_invocation_get_connection (invocation) ||
      g_strcmp0 (pid_data->client, g_dbus_method_invocation_get_sender (invocation)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNIX_PROCESS_ID_UNKNOWN,
                                             "No such pid");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_debug ("Sending signal %d to client pid %d", arg_signal, arg_pid);

  if (arg_to_process_group)
    killpg (pid_data->pid, arg_signal);
  else
    kill (pid_data->pid, arg_signal);

  pv_launcher1_complete_send_signal (self->launcher, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_terminate (PvLauncher1           *object,
                  GDBusMethodInvocation *invocation,
                  PvLauncherServer      *self)
{
  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (self),
                        G_DBUS_METHOD_INVOCATION_UNHANDLED);
  pv_launcher_server_terminate_children (self, SIGTERM);
  pv_launcher1_complete_terminate (object, invocation);
  pv_launcher_server_unref_skeleton_in_timeout (self);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  PvLauncherServer *self = user_data;
  const char *name, *from, *to;

  g_return_if_fail (PV_IS_LAUNCHER_SERVER (self));
  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      GHashTableIter iter;
      PidData *pid_data = NULL;
      gpointer value = NULL;
      GList *list = NULL, *l;

      g_hash_table_iter_init (&iter, self->client_pid_data_hash);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        {
          pid_data = value;

          if (g_str_equal (pid_data->client, name))
            list = g_list_prepend (list, pid_data);
        }

      for (l = list; l; l = l->next)
        {
          pid_data = l->data;
          g_debug ("%s dropped off the bus, killing %d", pid_data->client, pid_data->pid);
          killpg (pid_data->pid, SIGINT);
        }

      g_list_free (list);
    }
}

static gboolean
pv_launcher_server_finish_startup (PvLauncherServer *self,
                                   const char *bus_name,
                                   GError **error)
{
  self->exit_status = 0;
  pv_portal_listener_close_info_fh (self->listener, bus_name);
  return TRUE;
}

static gboolean
pv_launcher_server_export (PvLauncherServer *self,
                           GDBusConnection *connection,
                           GError **error)
{
  if (self->launcher == NULL)
    {
      self->launcher = pv_launcher1_skeleton_new ();

      g_object_set_data_full (G_OBJECT (self->launcher), "track-alive",
                              /* paired with unref in skeleton_died_cb */
                              g_object_ref (self),
                              skeleton_died_cb);

      pv_launcher1_set_version (PV_LAUNCHER1 (self->launcher), 0);
      pv_launcher1_set_supported_launch_flags (PV_LAUNCHER1 (self->launcher),
                                               PV_LAUNCH_FLAGS_MASK);

      g_signal_connect_object (self->launcher, "handle-launch",
                               G_CALLBACK (handle_launch), self,
                               CONNECT_FLAGS_NONE);
      g_signal_connect_object (self->launcher, "handle-send-signal",
                               G_CALLBACK (handle_send_signal), self,
                               CONNECT_FLAGS_NONE);
      g_signal_connect_object (self->launcher, "handle-terminate",
                               G_CALLBACK (handle_terminate), self,
                               CONNECT_FLAGS_NONE);
    }

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->launcher),
                                         connection,
                                         LAUNCHER_PATH,
                                         error))
    return FALSE;

  return TRUE;
}

static void
on_bus_acquired (PvPortalListener *listener,
                 GDBusConnection *connection,
                 gpointer user_data)
{
  PvLauncherServer *server = user_data;
  g_autoptr(GError) error = NULL;

  g_debug ("Bus acquired, creating skeleton");

  g_dbus_connection_set_exit_on_close (connection, FALSE);
  g_dbus_connection_signal_subscribe (connection,
                                      DBUS_NAME_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "NameOwnerChanged",
                                      DBUS_PATH_DBUS,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      server, NULL);

  if (!pv_launcher_server_export (server, connection, &error))
    {
      _srt_log_failure ("Unable to export object: %s", error->message);
      server->exit_status = EX_SOFTWARE;
      g_main_loop_quit (server->main_loop);
    }
}

static void
on_name_acquired (PvPortalListener *listener,
                  GDBusConnection *connection,
                  const gchar *name,
                  gpointer user_data)
{
  PvLauncherServer *server = user_data;

  g_debug ("Name acquired");

  /* If exporting the launcher didn't fail, then we are now happy */
  if (server->exit_status == EX_UNAVAILABLE)
    {
      g_autoptr(GError) error = NULL;

      if (!pv_launcher_server_finish_startup (server, name, &error))
        {
          _srt_log_failure ("%s", error->message);
          g_main_loop_quit (server->main_loop);
        }
    }
}

static void
on_name_lost (PvPortalListener *listener,
              GDBusConnection *connection,
              const gchar *name,
              gpointer user_data)
{
  PvLauncherServer *server = user_data;

  g_return_if_fail (PV_IS_LAUNCHER_SERVER (server));
  g_debug ("Name lost");
  pv_launcher_server_unref_skeleton_in_timeout (server);
}

/*
 * Callback for GDBusConnection::closed.
 */
static void
peer_connection_closed_cb (GDBusConnection *connection,
                           gboolean remote_peer_vanished,
                           GError *error,
                           gpointer user_data)
{
  PvLauncherServer *server = user_data;

  g_return_if_fail (PV_IS_LAUNCHER_SERVER (server));
  /* Paired with g_object_ref() in new_connection_cb() */
  g_object_unref (connection);
}

static gboolean
new_connection_cb (PvPortalListener *listener,
                   GDBusConnection *connection,
                   gpointer user_data)
{
  PvLauncherServer *server = user_data;
  GError *error = NULL;

  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (server), G_DBUS_METHOD_INVOCATION_HANDLED);
  /* Paired with g_object_unref() in peer_connection_closed_cb() */
  g_object_ref (connection);
  g_signal_connect_object (connection, "closed",
                           G_CALLBACK (peer_connection_closed_cb), server,
                           CONNECT_FLAGS_NONE);

  if (!pv_launcher_server_export (server, connection, &error))
    {
      g_warning ("Unable to export object: %s", error->message);
      g_dbus_connection_close (connection, NULL, NULL, NULL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
signal_handler (int sfd,
                G_GNUC_UNUSED GIOCondition condition,
                gpointer data)
{
  PvLauncherServer *server = data;
  struct signalfd_siginfo info;
  ssize_t size;

  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (server), G_SOURCE_CONTINUE);

  size = read (sfd, &info, sizeof (info));

  if (size < 0)
    {
      if (errno != EINTR && errno != EAGAIN)
        g_warning ("Unable to read struct signalfd_siginfo: %s",
                   g_strerror (errno));
    }
  else if (size != sizeof (info))
    {
      g_warning ("Expected struct signalfd_siginfo of size %"
                 G_GSIZE_FORMAT ", got %" G_GSSIZE_FORMAT,
                 sizeof (info), size);
    }
  else
    {
      pv_launcher_server_terminate_children (server, info.ssi_signo);

      if (server->main_loop != NULL)
        g_main_loop_quit (server->main_loop);
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
pv_launcher_server_connect_to_signals (PvLauncherServer *self,
                                       GError **error)
{
  static int signals[] = { SIGHUP, SIGINT, SIGTERM };
  sigset_t mask;
  guint i;
  int sfd;

  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (self), FALSE);
  sigemptyset (&mask);

  for (i = 0; i < G_N_ELEMENTS (signals); i++)
    sigaddset (&mask, signals[i]);

  sfd = signalfd (-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

  if (sfd < 0)
    {
      glnx_throw_errno_prefix (error, "Unable to watch signals");
      return FALSE;
    }

  /*
   * We have to block the signals, for two reasons:
   * - If we didn't, most of them would kill our process.
   *   Listening for a signal with a signalfd does not prevent the signal's
   *   default disposition from being acted on.
   * - Reading from a signalfd only returns information about the signals
   *   that are still pending for the process. If we ignored them instead
   *   of blocking them, they would no longer be pending by the time the
   *   main loop wakes up and reads from the signalfd.
   */
  pthread_sigmask (SIG_BLOCK, &mask, NULL);

  self->signals_id = g_unix_fd_add_full (G_PRIORITY_DEFAULT, sfd, G_IO_IN,
                                         signal_handler,
                                         g_object_ref (self), g_object_unref);
  return TRUE;
}

/*
 * If @fd is `stdin`, make `stdin` point to /dev/null and return a
 * new fd that is a duplicate of the original `stdin`, so that the
 * `stdin` inherited by child processes will not collide with the fd
 * we are using for some other purpose.
 */
static int
avoid_stdin (int fd,
             GError **error)
{
  g_return_val_if_fail (fd >= 0, FALSE);

  if (fd == STDIN_FILENO)
    {
      glnx_autofd int old_stdin = -1;
      glnx_autofd int new_stdin = -1;
      int fd_flags;

      old_stdin = dup (STDIN_FILENO);

      if (old_stdin < 0)
        {
          glnx_throw_errno_prefix (error,
                                   "Unable to duplicate standard input");
          return -1;
        }

      fd_flags = fcntl (old_stdin, F_GETFD);

      if (fd_flags < 0 ||
          fcntl (old_stdin, F_SETFD, fd_flags | FD_CLOEXEC) != 0)
        {
          glnx_throw_errno_prefix (error, "Unable to set flags on fd %d",
                                   old_stdin);
          return -1;
        }

      new_stdin = open ("/dev/null", O_RDONLY | O_CLOEXEC);

      if (new_stdin < 0)
        {
          glnx_throw_errno_prefix (error, "Unable to open /dev/null");
          return -1;
        }

      if (dup2 (new_stdin, STDIN_FILENO) != STDIN_FILENO)
        {
          glnx_throw_errno_prefix (error,
                                   "Unable to make stdin point to /dev/null");
          return -1;
        }

      fd = glnx_steal_fd (&old_stdin);
    }

  return fd;
}

static gboolean
exit_on_readable_cb (int fd,
                     GIOCondition condition,
                     gpointer user_data)
{
  PvLauncherServer *server = user_data;

  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (server), G_SOURCE_REMOVE);
  pv_launcher_server_terminate_children (server, SIGTERM);

  if (server->main_loop != NULL)
    g_main_loop_quit (server->main_loop);

  server->exit_on_readable_id = 0;
  return G_SOURCE_REMOVE;
}

static gboolean
pv_launcher_server_set_up_exit_on_readable (PvLauncherServer *server,
                                            int fd,
                                            GError **error)
{
  g_return_val_if_fail (PV_IS_LAUNCHER_SERVER (server), FALSE);
  g_return_val_if_fail (fd >= 0, FALSE);
  g_return_val_if_fail (server->exit_on_readable_id == 0, FALSE);

  if (fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
      return glnx_throw (error,
                         "--exit-on-readable fd cannot be stdout or stderr");
    }

  fd = avoid_stdin (fd, error);

  if (fd < 0)
    return FALSE;

  server->exit_on_readable_id = g_unix_fd_add_full (G_PRIORITY_DEFAULT,
                                                    fd, G_IO_IN|G_IO_ERR|G_IO_HUP,
                                                    exit_on_readable_cb,
                                                    g_object_ref (server),
                                                    g_object_unref);
  return TRUE;
}

static gchar *opt_bus_name = NULL;
static gint opt_exit_on_readable_fd = -1;
static gint opt_info_fd = -1;
static gboolean opt_replace = FALSE;
static gchar *opt_socket = NULL;
static gchar *opt_socket_directory = NULL;
static gboolean opt_verbose = FALSE;
static gboolean opt_version = FALSE;

static GOptionEntry options[] =
{
  { "bus-name", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_bus_name,
    "Use this well-known name on the D-Bus session bus.",
    "NAME" },
  { "exit-on-readable", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &opt_exit_on_readable_fd,
    "Exit when data is available for reading or when end-of-file is "
    "reached on this fd, usually 0 for stdin.",
    "FD" },
  { "info-fd", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_INT, &opt_info_fd,
    "Indicate readiness and print details of how to connect on this "
    "file descriptor instead of stdout.",
    "FD" },
  { "replace", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_replace,
    "Replace a previous instance with the same bus name. "
    "Ignored if --bus-name is not used.",
    NULL },
  { "socket", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_socket,
    "Listen on this AF_UNIX socket.",
    "ABSPATH|@ABSTRACT" },
  { "socket-directory", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_socket_directory,
    "Listen on an arbitrary AF_UNIX socket in this directory. "
    "Print the filename (socket=/path/to/socket), the "
    "D-Bus address (dbus_address=unix:...) and possibly other "
    "fields on stdout, one per line.",
    "PATH" },
  { "verbose", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_verbose,
    "Be more verbose.", NULL },
  { "version", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_version,
    "Print version number and exit.", NULL },
  { NULL }
};

int
main (int argc,
      char *argv[])
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  GBusNameOwnerFlags flags;
  int result;
  PvLauncherServer *server = g_object_new (PV_TYPE_LAUNCHER_SERVER, NULL);

  server->exit_status = EX_USAGE;
  server->listener = pv_portal_listener_new ();
  server->main_loop = g_main_loop_new (NULL, FALSE);

  setlocale (LC_ALL, "");

  g_set_prgname ("pressure-vessel-launcher");

  /* Set up the initial base logging */
  _srt_util_set_glib_log_handler (FALSE);

  context = g_option_context_new ("");
  g_option_context_set_summary (context,
                                "Accept IPC requests to create child "
                                "processes.");

  g_option_context_add_main_entries (context, options, NULL);
  opt_verbose = _srt_boolean_environment ("PRESSURE_VESSEL_VERBOSE", FALSE);

  if (!g_option_context_parse (context, &argc, &argv, error))
    {
      server->exit_status = EX_USAGE;
      goto out;
    }

  if (opt_version)
    {
      g_print ("%s:\n"
               " Package: pressure-vessel\n"
               " Version: %s\n",
               g_get_prgname (), VERSION);
      server->exit_status = 0;
      goto out;
    }

  if (opt_verbose)
    _srt_util_set_glib_log_handler (opt_verbose);

  if ((result = _srt_set_compatible_resource_limits (0)) < 0)
    g_warning ("Unable to set normal resource limits: %s",
               g_strerror (-result));

  /* opt_info_fd defaults to stdout */
  if (opt_info_fd < 0)
    opt_info_fd = STDOUT_FILENO;

  if (!pv_portal_listener_set_up_info_fd (server->listener,
                                          opt_info_fd,
                                          error))
    {
      server->exit_status = EX_OSERR;
      goto out;
    }

  if (opt_exit_on_readable_fd >= 0)
    {
      if (!pv_launcher_server_set_up_exit_on_readable (server,
                                                       opt_exit_on_readable_fd,
                                                       error))
        {
          server->exit_status = EX_OSERR;
          goto out;
        }
    }

  /* We have to block the signals we want to forward before we start any
   * other thread, and in particular the GDBus worker thread, because
   * the signal mask is per-thread. We need all threads to have the same
   * mask, otherwise a thread that doesn't have the mask will receive
   * process-directed signals, causing the whole process to exit. */
  if (!pv_launcher_server_connect_to_signals (server, error))
    {
      server->exit_status = EX_OSERR;
      goto out;
    }

  _srt_setenv_disable_gio_modules ();

  if (argc >= 2 && strcmp (argv[1], "--") == 0)
    {
      argv++;
      argc--;
    }

  if (argc != 1)
    {
      glnx_throw (error, "Usage: %s [OPTIONS]", g_get_prgname ());
      goto out;
    }

  server->client_pid_data_hash = g_hash_table_new_full (NULL, NULL, NULL,
                                                        (GDestroyNotify) pid_data_free);

  if (!pv_portal_listener_check_socket_arguments (server->listener,
                                                  opt_bus_name,
                                                  opt_socket,
                                                  opt_socket_directory,
                                                  error))
    goto out;

  /* Exit with this status until we know otherwise */
  server->exit_status = EX_SOFTWARE;

  g_signal_connect_object (server->listener,
                           "new-peer-connection", G_CALLBACK (new_connection_cb),
                           server, CONNECT_FLAGS_NONE);
  g_signal_connect_object (server->listener,
                           "session-bus-connected", G_CALLBACK (on_bus_acquired),
                           server, CONNECT_FLAGS_NONE);
  g_signal_connect_object (server->listener,
                           "session-bus-name-acquired", G_CALLBACK (on_name_acquired),
                           server, CONNECT_FLAGS_NONE);
  g_signal_connect_object (server->listener,
                           "session-bus-name-lost", G_CALLBACK (on_name_lost),
                           server, CONNECT_FLAGS_NONE);

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;

  if (opt_replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  if (!pv_portal_listener_listen (server->listener,
                                  opt_bus_name,
                                  flags,
                                  opt_socket,
                                  opt_socket_directory,
                                  error))
    goto out;

  server->exit_status = EX_UNAVAILABLE;

  /* If we're using the bus name method, we can't exit successfully
   * until we claimed the bus name at least once: see on_name_acquired().
   * Otherwise we're already content. */
  if (opt_bus_name == NULL)
    {
      if (!pv_launcher_server_finish_startup (server, NULL, error))
        goto out;
    }

  g_debug ("Entering main loop");

  g_main_loop_run (server->main_loop);

out:
  if (local_error != NULL)
    _srt_log_failure ("%s", local_error->message);

  g_free (opt_bus_name);
  g_free (opt_socket);
  g_free (opt_socket_directory);

  if (server->exit_status == -1)
    {
      if (local_error == NULL)
        server->exit_status = 0;
      else if (local_error->domain == G_OPTION_ERROR)
        server->exit_status = EX_USAGE;
      else
        server->exit_status = EX_UNAVAILABLE;
    }

  pv_launcher_server_stop (server);
  result = server->exit_status;
  g_clear_object (&server);
  g_clear_error (&local_error);

  g_debug ("Exiting with status %d", result);
  return result;
}
