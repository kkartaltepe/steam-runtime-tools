/* pressure-vessel-wrap — run a program in a container that protects $HOME,
 * optionally using a Flatpak-style runtime.
 *
 * Contains code taken from Flatpak.
 *
 * Copyright © 2014-2019 Red Hat, Inc
 * Copyright © 2017-2020 Collabora Ltd.
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
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <locale.h>
#include <stdlib.h>

#include "glib-backports.h"
#include "libglnx/libglnx.h"

#include "bwrap.h"
#include "bwrap-lock.h"
#include "flatpak-bwrap-private.h"
#include "flatpak-run-private.h"
#include "flatpak-utils-base-private.h"
#include "flatpak-utils-private.h"
#include "runtime.h"
#include "utils.h"
#include "wrap-interactive.h"

static gchar *
find_executable_dir (GError **error)
{
  g_autofree gchar *target = glnx_readlinkat_malloc (-1, "/proc/self/exe",
                                                     NULL, error);

  if (target == NULL)
    return glnx_prefix_error_null (error, "Unable to resolve /proc/self/exe");

  return g_path_get_dirname (target);
}

static gchar *
find_bwrap (const char *tools_dir)
{
  static const char * const flatpak_libexecdirs[] =
  {
    "/usr/local/libexec",
    "/usr/libexec",
    "/usr/lib/flatpak"
  };
  const char *tmp;
  g_autofree gchar *candidate = NULL;
  gsize i;

  g_return_val_if_fail (tools_dir != NULL, NULL);

  tmp = g_getenv ("BWRAP");

  if (tmp != NULL)
    return g_strdup (tmp);

  candidate = g_find_program_in_path ("bwrap");

  if (candidate != NULL)
    return g_steal_pointer (&candidate);

  for (i = 0; i < G_N_ELEMENTS (flatpak_libexecdirs); i++)
    {
      candidate = g_build_filename (flatpak_libexecdirs[i],
                                    "flatpak-bwrap", NULL);

      if (g_file_test (candidate, G_FILE_TEST_IS_EXECUTABLE))
        return g_steal_pointer (&candidate);
      else
        g_clear_pointer (&candidate, g_free);
    }

  candidate = g_build_filename (tools_dir, "bwrap", NULL);

  if (g_file_test (candidate, G_FILE_TEST_IS_EXECUTABLE))
    return g_steal_pointer (&candidate);
  else
    g_clear_pointer (&candidate, g_free);

  return NULL;
}

static gchar *
check_bwrap (const char *tools_dir,
             gboolean only_prepare)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  g_autofree gchar *bwrap_executable = NULL;
  const char *bwrap_test_argv[] =
  {
    NULL,
    "--bind", "/", "/",
    "true",
    NULL
  };

  g_return_val_if_fail (tools_dir != NULL, NULL);

  bwrap_executable = find_bwrap (tools_dir);

  if (bwrap_executable == NULL)
    {
      g_warning ("Cannot find bwrap");
    }
  else if (only_prepare)
    {
      /* With --only-prepare we don't necessarily expect to be able to run
       * it anyway (we are probably in a Docker container that doesn't allow
       * creation of nested user namespaces), so just assume that it's the
       * right one. */
      return g_steal_pointer (&bwrap_executable);
    }
  else
    {
      int wait_status;
      g_autofree gchar *child_stdout = NULL;
      g_autofree gchar *child_stderr = NULL;

      bwrap_test_argv[0] = bwrap_executable;

      /* We use LEAVE_DESCRIPTORS_OPEN to work around a deadlock in older GLib,
       * see flatpak_close_fds_workaround */
      if (!g_spawn_sync (NULL,  /* cwd */
                         (gchar **) bwrap_test_argv,
                         NULL,  /* environ */
                         G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                         flatpak_bwrap_child_setup_cb, NULL,
                         &child_stdout,
                         &child_stderr,
                         &wait_status,
                         error))
        {
          g_warning ("Cannot run bwrap: %s", local_error->message);
          g_clear_error (&local_error);
        }
      else if (wait_status != 0)
        {
          g_warning ("Cannot run bwrap: wait status %d", wait_status);

          if (child_stdout != NULL && child_stdout[0] != '\0')
            g_warning ("Output:\n%s", child_stdout);

          if (child_stderr != NULL && child_stderr[0] != '\0')
            g_warning ("Diagnostic output:\n%s", child_stderr);
        }
      else
        {
          return g_steal_pointer (&bwrap_executable);
        }
    }

  return NULL;
}

static gchar *
check_flatpak_spawn (void)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  g_autofree gchar *spawn_exec = NULL;
  g_autofree gchar *child_stdout = NULL;
  g_autofree gchar *child_stderr = NULL;
  int wait_status;
  const char *spawn_test_argv[] =
  {
    NULL,
    "--host",
    "--directory=/",
    "true",
    NULL
  };

  /* All known Flatpak runtimes have flatpak-spawn in the PATH */
  spawn_exec = g_find_program_in_path ("flatpak-spawn");

  if (spawn_exec == NULL
      || !g_file_test (spawn_exec, G_FILE_TEST_IS_EXECUTABLE))
    return NULL;

  spawn_test_argv[0] = spawn_exec;

  if (!g_spawn_sync (NULL,  /* cwd */
                     (gchar **) spawn_test_argv,
                     NULL,  /* environ */
                     G_SPAWN_DEFAULT,
                     NULL, NULL,    /* child setup */
                     &child_stdout,
                     &child_stderr,
                     &wait_status,
                     error))
    {
      g_warning ("Cannot run flatpak-spawn: %s", local_error->message);
      g_clear_error (&local_error);
    }
  else if (wait_status != 0)
    {
      g_warning ("Cannot run flatpak-spawn: wait status %d", wait_status);

      if (child_stdout != NULL && child_stdout[0] != '\0')
        g_warning ("Output:\n%s", child_stdout);

      if (child_stderr != NULL && child_stderr[0] != '\0')
        g_warning ("Diagnostic output:\n%s", child_stderr);
    }
  else
    {
      return g_steal_pointer (&spawn_exec);
    }

  return NULL;
}

/*
 * Export most root directories, but not the ones that
 * "flatpak run --filesystem=host" would skip.
 * (See flatpak_context_export(), which might replace this function
 * later on.)
 *
 * If we are running inside Flatpak, we assume that any directory
 * that is made available in the root, and is not in dont_mount_in_root,
 * came in via --filesystem=host or similar and matches its equivalent
 * on the real root filesystem.
 */
static gboolean
export_root_dirs_like_filesystem_host (FlatpakExports *exports,
                                       FlatpakFilesystemMode mode,
                                       GError **error)
{
  g_autoptr(GDir) dir = NULL;
  const char *member = NULL;

  g_return_val_if_fail (exports != NULL, FALSE);
  g_return_val_if_fail ((unsigned) mode <= FLATPAK_FILESYSTEM_MODE_LAST, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  dir = g_dir_open ("/", 0, error);

  if (dir == NULL)
    return FALSE;

  for (member = g_dir_read_name (dir);
       member != NULL;
       member = g_dir_read_name (dir))
    {
      g_autofree gchar *path = NULL;

      if (g_strv_contains (dont_mount_in_root, member))
        continue;

      path = g_build_filename ("/", member, NULL);
      flatpak_exports_add_path_expose (exports, mode, path);
    }

  /* For parity with Flatpak's handling of --filesystem=host */
  flatpak_exports_add_path_expose (exports, mode, "/run/media");

  return TRUE;
}

/*
 * This function assumes that /run on the host is the same as in the
 * current namespace, so it won't work in Flatpak.
 */
static gboolean
export_contents_of_run (FlatpakBwrap *bwrap,
                        GError **error)
{
  static const char *ignore[] =
  {
    "gfx",              /* can be created by pressure-vessel */
    "host",             /* created by pressure-vessel */
    "media",            /* see export_root_dirs_like_filesystem_host() */
    "pressure-vessel",  /* created by pressure-vessel */
    NULL
  };
  g_autoptr(GDir) dir = NULL;
  const char *member = NULL;

  g_return_val_if_fail (bwrap != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS),
                        FALSE);

  dir = g_dir_open ("/run", 0, error);

  if (dir == NULL)
    return FALSE;

  for (member = g_dir_read_name (dir);
       member != NULL;
       member = g_dir_read_name (dir))
    {
      g_autofree gchar *path = NULL;

      if (g_strv_contains (ignore, member))
        continue;

      path = g_build_filename ("/run", member, NULL);
      flatpak_bwrap_add_args (bwrap,
                              "--bind", path, path,
                              NULL);
    }

  return TRUE;
}

typedef enum
{
  ENV_MOUNT_FLAGS_COLON_DELIMITED = (1 << 0),
  ENV_MOUNT_FLAGS_DEPRECATED = (1 << 1),
  ENV_MOUNT_FLAGS_NONE = 0
} EnvMountFlags;

typedef struct
{
  const char *name;
  EnvMountFlags flags;
} EnvMount;

static const EnvMount known_required_env[] =
{
    { "STEAM_COMPAT_APP_LIBRARY_PATH", ENV_MOUNT_FLAGS_DEPRECATED },
    { "STEAM_COMPAT_APP_LIBRARY_PATHS",
      ENV_MOUNT_FLAGS_COLON_DELIMITED | ENV_MOUNT_FLAGS_DEPRECATED },
    { "STEAM_COMPAT_CLIENT_INSTALL_PATH", ENV_MOUNT_FLAGS_NONE },
    { "STEAM_COMPAT_DATA_PATH", ENV_MOUNT_FLAGS_NONE },
    { "STEAM_COMPAT_INSTALL_PATH", ENV_MOUNT_FLAGS_NONE },
    { "STEAM_COMPAT_LIBRARY_PATHS", ENV_MOUNT_FLAGS_COLON_DELIMITED },
    { "STEAM_COMPAT_MOUNT_PATHS",
      ENV_MOUNT_FLAGS_COLON_DELIMITED | ENV_MOUNT_FLAGS_DEPRECATED },
    { "STEAM_COMPAT_MOUNTS", ENV_MOUNT_FLAGS_COLON_DELIMITED },
    { "STEAM_COMPAT_SHADER_PATH", ENV_MOUNT_FLAGS_NONE },
    { "STEAM_COMPAT_TOOL_PATH", ENV_MOUNT_FLAGS_DEPRECATED },
    { "STEAM_COMPAT_TOOL_PATHS", ENV_MOUNT_FLAGS_COLON_DELIMITED },
};

static void
bind_and_propagate_from_environ (FlatpakExports *exports,
                                 FlatpakBwrap *bwrap,
                                 FlatpakFilesystemMode mode,
                                 const char *variable,
                                 EnvMountFlags flags)
{
  g_auto(GStrv) values = NULL;
  const char *value;
  const char *before;
  const char *after;
  gboolean changed = FALSE;
  gsize i;

  g_return_if_fail (exports != NULL);
  g_return_if_fail ((unsigned) mode <= FLATPAK_FILESYSTEM_MODE_LAST);
  g_return_if_fail (variable != NULL);

  value = g_getenv (variable);

  if (value == NULL)
    return;

  if (flags & ENV_MOUNT_FLAGS_DEPRECATED)
    g_message ("Setting $%s is deprecated", variable);

  if (flags & ENV_MOUNT_FLAGS_COLON_DELIMITED)
    {
      values = g_strsplit (value, ":", -1);
      before = "...:";
      after = ":...";
    }
  else
    {
      values = g_new0 (gchar *, 2);
      values[0] = g_strdup (value);
      values[1] = NULL;
      before = "";
      after = "";
    }

  for (i = 0; values[i] != NULL; i++)
    {
      g_autofree gchar *value_host = NULL;
      g_autofree gchar *canon = NULL;

      if (values[i][0] == '\0')
        continue;

      if (!g_file_test (values[i], G_FILE_TEST_EXISTS))
        {
          g_debug ("Not bind-mounting %s=\"%s%s%s\" because it does not exist",
                   variable, before, values[i], after);
          continue;
        }

      canon = g_canonicalize_filename (values[i], NULL);
      value_host = pv_current_namespace_path_to_host_path (canon);

      g_debug ("Bind-mounting %s=\"%s%s%s\" from the current env as %s=\"%s%s%s\" in the host",
               variable, before, values[i], after,
               variable, before, value_host, after);
      flatpak_exports_add_path_expose (exports, mode, canon);

      if (strcmp (values[i], value_host) != 0)
        {
          g_clear_pointer (&values[i], g_free);
          values[i] = g_steal_pointer (&value_host);
          changed = TRUE;
        }
    }

  if (changed)
    {
      g_autofree gchar *joined = g_strjoinv (":", values);

      flatpak_bwrap_add_args (bwrap,
                              "--setenv", variable, joined,
                              NULL);
    }
}

/* Order matters here: root, steam and steambeta are or might be symlinks
 * to the root of the Steam installation, so we want to bind-mount their
 * targets before we deal with the rest. */
static const char * const steam_api_subdirs[] =
{
  "root", "steam", "steambeta", "bin", "bin32", "bin64", "sdk32", "sdk64",
};

static gboolean
use_fake_home (FlatpakExports *exports,
               FlatpakBwrap *bwrap,
               const gchar *fake_home,
               GError **error)
{
  const gchar *real_home = g_get_home_dir ();
  g_autofree gchar *cache = g_build_filename (fake_home, ".cache", NULL);
  g_autofree gchar *cache2 = g_build_filename (fake_home, "cache", NULL);
  g_autofree gchar *tmp = g_build_filename (cache, "tmp", NULL);
  g_autofree gchar *config = g_build_filename (fake_home, ".config", NULL);
  g_autofree gchar *config2 = g_build_filename (fake_home, "config", NULL);
  g_autofree gchar *local = g_build_filename (fake_home, ".local", NULL);
  g_autofree gchar *data = g_build_filename (local, "share", NULL);
  g_autofree gchar *data2 = g_build_filename (fake_home, "data", NULL);
  g_autofree gchar *steam_pid = NULL;
  g_autofree gchar *steam_pipe = NULL;
  gsize i;

  g_return_val_if_fail (bwrap != NULL, FALSE);
  g_return_val_if_fail (exports != NULL, FALSE);
  g_return_val_if_fail (fake_home != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_mkdir_with_parents (fake_home, 0700);
  g_mkdir_with_parents (cache, 0700);
  g_mkdir_with_parents (tmp, 0700);
  g_mkdir_with_parents (config, 0700);
  g_mkdir_with_parents (local, 0700);
  g_mkdir_with_parents (data, 0700);

  if (!g_file_test (cache2, G_FILE_TEST_EXISTS))
    {
      g_unlink (cache2);

      if (symlink (".cache", cache2) != 0)
        return glnx_throw_errno_prefix (error,
                                        "Unable to create symlink %s -> .cache",
                                        cache2);
    }

  if (!g_file_test (config2, G_FILE_TEST_EXISTS))
    {
      g_unlink (config2);

      if (symlink (".config", config2) != 0)
        return glnx_throw_errno_prefix (error,
                                        "Unable to create symlink %s -> .config",
                                        config2);
    }

  if (!g_file_test (data2, G_FILE_TEST_EXISTS))
    {
      g_unlink (data2);

      if (symlink (".local/share", data2) != 0)
        return glnx_throw_errno_prefix (error,
                                        "Unable to create symlink %s -> .local/share",
                                        data2);
    }

  flatpak_bwrap_add_args (bwrap,
                          "--bind", fake_home, real_home,
                          "--bind", tmp, "/var/tmp",
                          "--setenv", "XDG_CACHE_HOME", cache,
                          "--setenv", "XDG_CONFIG_HOME", config,
                          "--setenv", "XDG_DATA_HOME", data,
                          NULL);

  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                   fake_home);

  /*
   * These might be API entry points, according to Steam/steam.sh.
   * They're usually symlinks into the Steam root, except for in
   * older steam Debian packages that had Debian bug #916303.
   *
   * TODO: We probably want to hide part or all of root, steam,
   * steambeta?
   */
  for (i = 0; i < G_N_ELEMENTS (steam_api_subdirs); i++)
    {
      g_autofree gchar *dir = g_build_filename (real_home, ".steam",
                                                steam_api_subdirs[i], NULL);
      g_autofree gchar *mount_point = g_build_filename (fake_home, ".steam",
                                                        steam_api_subdirs[i],
                                                        NULL);
      g_autofree gchar *target = NULL;

      target = glnx_readlinkat_malloc (-1, dir, NULL, NULL);

      if (target != NULL)
        {
          /* We used to bind-mount these directories, so transition them
           * to symbolic links if we can. */
          if (rmdir (mount_point) != 0 && errno != ENOENT && errno != ENOTDIR)
            g_debug ("rmdir %s: %s", mount_point, g_strerror (errno));

          /* Remove any symlinks that might have already been there. */
          if (unlink (mount_point) != 0 && errno != ENOENT)
            g_debug ("unlink %s: %s", mount_point, g_strerror (errno));
        }

      flatpak_exports_add_path_expose (exports,
                                       FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                       dir);
    }

  /* steamclient.so relies on this for communication with Steam */
  steam_pid = g_build_filename (real_home, ".steam", "steam.pid", NULL);
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   steam_pid);

  /* Make sure Steam IPC is available.
   * TODO: do we need this? do we need more? */
  steam_pipe = g_build_filename (real_home, ".steam", "steam.pipe", NULL);
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                   steam_pipe);

  return TRUE;
}

/*
 * @bwrap: Arguments produced by flatpak_exports_append_bwrap_args(),
 *  not including an executable name (the 0'th argument must be
 *  `--bind` or similar)
 * @home: The home directory
 *
 * Adjust arguments in @bwrap to cope with potentially running in a
 * container.
 */
static void
adjust_exports (FlatpakBwrap *bwrap,
                const char *home)
{
  gsize i = 0;

  while (i < bwrap->argv->len)
    {
      const char *opt = bwrap->argv->pdata[i];

      g_assert (opt != NULL);

      if (g_str_equal (opt, "--symlink"))
        {
          g_assert (i + 3 <= bwrap->argv->len);
          /* pdata[i + 1] is the target: unchanged. */
          /* pdata[i + 2] is a path in the final container: unchanged. */
          i += 3;
        }
      else if (g_str_equal (opt, "--dir") ||
               g_str_equal (opt, "--tmpfs"))
        {
          g_assert (i + 2 <= bwrap->argv->len);
          /* pdata[i + 1] is a path in the final container: unchanged. */
          i += 2;
        }
      else if (g_str_equal (opt, "--ro-bind") ||
               g_str_equal (opt, "--bind"))
        {
          g_autofree gchar *src = NULL;

          g_assert (i + 3 <= bwrap->argv->len);
          src = g_steal_pointer (&bwrap->argv->pdata[i + 1]);
          /* pdata[i + 2] is a path in the final container: unchanged. */

          /* Paths in the home directory might need adjusting.
           * Paths outside the home directory do not: if they're part of
           * /run/host, they've been adjusted already by
           * flatpak_exports_take_host_fd(), and if not, they appear in
           * the container with the same path as on the host. */
          if (flatpak_has_path_prefix (src, home))
            bwrap->argv->pdata[i + 1] = pv_current_namespace_path_to_host_path (src);
          else
            bwrap->argv->pdata[i + 1] = g_steal_pointer (&src);

          i += 3;
        }
      else
        {
          g_return_if_reached ();
        }
    }
}

typedef enum
{
  TRISTATE_NO = 0,
  TRISTATE_YES,
  TRISTATE_MAYBE
} Tristate;

static gboolean opt_batch = FALSE;
static char *opt_copy_runtime_into = NULL;
static char **opt_env_if_host = NULL;
static char *opt_fake_home = NULL;
static char **opt_filesystems = NULL;
static char *opt_freedesktop_app_id = NULL;
static char *opt_steam_app_id = NULL;
static gboolean opt_gc_runtimes = TRUE;
static gboolean opt_generate_locales = TRUE;
static char *opt_home = NULL;
static gboolean opt_host_fallback = FALSE;
static char *opt_graphics_provider = NULL;
static char *graphics_provider_mount_point = NULL;
static gboolean opt_only_prepare = FALSE;
static gboolean opt_remove_game_overlay = FALSE;
static PvShell opt_shell = PV_SHELL_NONE;
static GPtrArray *opt_ld_preload = NULL;
static GArray *opt_pass_fds = NULL;
static char *opt_runtime_base = NULL;
static char *opt_runtime = NULL;
static Tristate opt_share_home = TRISTATE_MAYBE;
static gboolean opt_share_pid = TRUE;
static double opt_terminate_idle_timeout = 0.0;
static double opt_terminate_timeout = -1.0;
static gboolean opt_verbose = FALSE;
static gboolean opt_version = FALSE;
static gboolean opt_version_only = FALSE;
static gboolean opt_test = FALSE;
static PvTerminal opt_terminal = PV_TERMINAL_AUTO;
static char *opt_write_bwrap = NULL;

static gboolean
opt_host_ld_preload_cb (const gchar *option_name,
                        const gchar *value,
                        gpointer data,
                        GError **error)
{
  gchar *preload = g_strdup_printf ("host:%s", value);

  if (opt_ld_preload == NULL)
    opt_ld_preload = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (opt_ld_preload, g_steal_pointer (&preload));

  return TRUE;
}

static gboolean
opt_pass_fd_cb (const char *name,
                const char *value,
                gpointer data,
                GError **error)
{
  char *endptr;
  gint64 i64 = g_ascii_strtoll (value, &endptr, 10);
  int fd;
  int fd_flags;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  if (i64 < 0 || i64 > G_MAXINT || endptr == value || *endptr != '\0')
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   "Integer out of range or invalid: %s", value);
      return FALSE;
    }

  fd = (int) i64;

  fd_flags = fcntl (fd, F_GETFD);

  if (fd_flags < 0)
    return glnx_throw_errno_prefix (error, "Unable to receive --fd %d", fd);

  if (opt_pass_fds == NULL)
    opt_pass_fds = g_array_new (FALSE, FALSE, sizeof (int));

  g_array_append_val (opt_pass_fds, fd);
  return TRUE;
}

static gboolean
opt_shell_cb (const gchar *option_name,
              const gchar *value,
              gpointer data,
              GError **error)
{
  if (g_strcmp0 (option_name, "--shell-after") == 0)
    value = "after";
  else if (g_strcmp0 (option_name, "--shell-fail") == 0)
    value = "fail";
  else if (g_strcmp0 (option_name, "--shell-instead") == 0)
    value = "instead";

  if (value == NULL || *value == '\0')
    {
      opt_shell = PV_SHELL_NONE;
      return TRUE;
    }

  switch (value[0])
    {
      case 'a':
        if (g_strcmp0 (value, "after") == 0)
          {
            opt_shell = PV_SHELL_AFTER;
            return TRUE;
          }
        break;

      case 'f':
        if (g_strcmp0 (value, "fail") == 0)
          {
            opt_shell = PV_SHELL_FAIL;
            return TRUE;
          }
        break;

      case 'i':
        if (g_strcmp0 (value, "instead") == 0)
          {
            opt_shell = PV_SHELL_INSTEAD;
            return TRUE;
          }
        break;

      case 'n':
        if (g_strcmp0 (value, "none") == 0 || g_strcmp0 (value, "no") == 0)
          {
            opt_shell = PV_SHELL_NONE;
            return TRUE;
          }
        break;

      default:
        /* fall through to error */
        break;
    }

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               "Unknown choice \"%s\" for %s", value, option_name);
  return FALSE;
}

static gboolean
opt_terminal_cb (const gchar *option_name,
                 const gchar *value,
                 gpointer data,
                 GError **error)
{
  if (g_strcmp0 (option_name, "--tty") == 0)
    value = "tty";
  else if (g_strcmp0 (option_name, "--xterm") == 0)
    value = "xterm";

  if (value == NULL || *value == '\0')
    {
      opt_terminal = PV_TERMINAL_AUTO;
      return TRUE;
    }

  switch (value[0])
    {
      case 'a':
        if (g_strcmp0 (value, "auto") == 0)
          {
            opt_terminal = PV_TERMINAL_AUTO;
            return TRUE;
          }
        break;

      case 'n':
        if (g_strcmp0 (value, "none") == 0 || g_strcmp0 (value, "no") == 0)
          {
            opt_terminal = PV_TERMINAL_NONE;
            return TRUE;
          }
        break;

      case 't':
        if (g_strcmp0 (value, "tty") == 0)
          {
            opt_terminal = PV_TERMINAL_TTY;
            return TRUE;
          }
        break;

      case 'x':
        if (g_strcmp0 (value, "xterm") == 0)
          {
            opt_terminal = PV_TERMINAL_XTERM;
            return TRUE;
          }
        break;

      default:
        /* fall through to error */
        break;
    }

  g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
               "Unknown choice \"%s\" for %s", value, option_name);
  return FALSE;
}

static gboolean
opt_share_home_cb (const gchar *option_name,
                   const gchar *value,
                   gpointer data,
                   GError **error)
{
  if (g_strcmp0 (option_name, "--share-home") == 0)
    opt_share_home = TRISTATE_YES;
  else if (g_strcmp0 (option_name, "--unshare-home") == 0)
    opt_share_home = TRISTATE_NO;
  else
    g_return_val_if_reached (FALSE);

  return TRUE;
}

static gboolean
opt_with_host_graphics_cb (const gchar *option_name,
                           const gchar *value,
                           gpointer data,
                           GError **error)
{
  /* This is the old way to get the graphics from the host system */
  if (g_strcmp0 (option_name, "--with-host-graphics") == 0)
    {
      if (g_file_test ("/run/host", G_FILE_TEST_IS_DIR))
        opt_graphics_provider = g_strdup ("/run/host");
      else
        opt_graphics_provider = g_strdup ("/");
    }
  /* This is the old way to avoid using graphics from the host */
  else if (g_strcmp0 (option_name, "--without-host-graphics") == 0)
    {
      opt_graphics_provider = g_strdup ("");
    }
  else
    {
      g_return_val_if_reached (FALSE);
    }

  g_warning ("\"--with-host-graphics\" and \"--without-host-graphics\" have "
             "been deprecated and could be removed in future releases. Please use "
             "use \"--graphics-provider=/\", \"--graphics-provider=/run/host\" or "
             "\"--graphics-provider=\" instead.");

  return TRUE;
}

static GOptionEntry options[] =
{
  { "batch", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_batch,
    "Disable all interactivity and redirection: ignore --shell*, "
    "--terminal, --xterm, --tty. [Default: if $PRESSURE_VESSEL_BATCH]", NULL },
  { "copy-runtime-into", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_copy_runtime_into,
    "If a --runtime is used, copy it into DIR and edit the copy in-place. "
    "[Default: $PRESSURE_VESSEL_COPY_RUNTIME_INTO or empty]",
    "DIR" },
  { "env-if-host", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING_ARRAY, &opt_env_if_host,
    "Set VAR=VAL if COMMAND is run with /usr from the host system, "
    "but not if it is run with /usr from RUNTIME.", "VAR=VAL" },
  { "filesystem", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME_ARRAY, &opt_filesystems,
    "Share filesystem directories with the container. "
    "They must currently be given as absolute paths.",
    "PATH" },
  { "freedesktop-app-id", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_freedesktop_app_id,
    "Make --unshare-home use ~/.var/app/ID as home directory, where ID "
    "is com.example.MyApp or similar. This interoperates with Flatpak. "
    "[Default: $PRESSURE_VESSEL_FDO_APP_ID if set]",
    "ID" },
  { "steam-app-id", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &opt_steam_app_id,
    "Make --unshare-home use ~/.var/app/com.steampowered.AppN "
    "as home directory. [Default: $STEAM_COMPAT_APP_ID or $SteamAppId]",
    "N" },
  { "gc-runtimes", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_gc_runtimes,
    "If using --copy-runtime-into, garbage-collect old temporary "
    "runtimes. [Default, unless $PRESSURE_VESSEL_GC_RUNTIMES is 0]",
    NULL },
  { "no-gc-runtimes", '\0',
    G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_gc_runtimes,
    "If using --copy-runtime-into, don't garbage-collect old "
    "temporary runtimes.", NULL },
  { "generate-locales", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_generate_locales,
    "If using --runtime, attempt to generate any missing locales. "
    "[Default, unless $PRESSURE_VESSEL_GENERATE_LOCALES is 0]",
    NULL },
  { "no-generate-locales", '\0',
    G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_generate_locales,
    "If using --runtime, don't generate any missing locales.", NULL },
  { "home", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_home,
    "Use HOME as home directory. Implies --unshare-home. "
    "[Default: $PRESSURE_VESSEL_HOME if set]", "HOME" },
  { "host-fallback", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_host_fallback,
    "Run COMMAND on the host system if we cannot run it in a container.", NULL },
  { "host-ld-preload", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, &opt_host_ld_preload_cb,
    "Add MODULE from the host system to LD_PRELOAD when executing COMMAND.",
    "MODULE" },
  { "graphics-provider", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_graphics_provider,
    "If using --runtime, use PATH as the graphics provider. "
    "The path is assumed to be relative to the current namespace, "
    "and will be adjusted for use on the host system if pressure-vessel "
    "is run in a container. The empty string means use the graphics "
    "stack from container."
    "[Default: $PRESSURE_VESSEL_GRAPHICS_PROVIDER or '/run/host' or '/']", "PATH" },
  { "pass-fd", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, opt_pass_fd_cb,
    "Let the launched process inherit the given fd.",
    NULL },
  { "remove-game-overlay", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_remove_game_overlay,
    "Disable the Steam Overlay. "
    "[Default if $PRESSURE_VESSEL_REMOVE_GAME_OVERLAY is 1]",
    NULL },
  { "keep-game-overlay", '\0',
    G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_remove_game_overlay,
    "Do not disable the Steam Overlay. "
    "[Default unless $PRESSURE_VESSEL_REMOVE_GAME_OVERLAY is 1]",
    NULL },
  { "runtime", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_runtime,
    "Mount the given sysroot or merged /usr in the container, and augment "
    "it with the provider's graphics stack. The empty string "
    "means don't use a runtime. [Default: $PRESSURE_VESSEL_RUNTIME or '']",
    "RUNTIME" },
  { "runtime-base", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME, &opt_runtime_base,
    "If a --runtime is a relative path, look for it relative to BASE. "
    "[Default: $PRESSURE_VESSEL_RUNTIME_BASE or '.']",
    "BASE" },
  { "share-home", '\0',
    G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, opt_share_home_cb,
    "Use the real home directory. "
    "[Default unless $PRESSURE_VESSEL_HOME is set or "
    "$PRESSURE_VESSEL_SHARE_HOME is 0]",
    NULL },
  { "unshare-home", '\0',
    G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, opt_share_home_cb,
    "Use an app-specific home directory chosen according to --home, "
    "--freedesktop-app-id, --steam-app-id or $STEAM_COMPAT_APP_ID. "
    "[Default if $PRESSURE_VESSEL_HOME is set or "
    "$PRESSURE_VESSEL_SHARE_HOME is 0]",
    NULL },
  { "share-pid", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_share_pid,
    "Do not create a new process ID namespace for the app. "
    "[Default, unless $PRESSURE_VESSEL_SHARE_PID is 0]",
    NULL },
  { "unshare-pid", '\0',
    G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_share_pid,
    "Create a new process ID namespace for the app. "
    "[Default if $PRESSURE_VESSEL_SHARE_PID is 0]",
    NULL },
  { "shell", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, opt_shell_cb,
    "--shell=after is equivalent to --shell-after, and so on. "
    "[Default: $PRESSURE_VESSEL_SHELL or 'none']",
    "{none|after|fail|instead}" },
  { "shell-after", '\0',
    G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, opt_shell_cb,
    "Run an interactive shell after COMMAND. Executing \"$@\" in that "
    "shell will re-run COMMAND [ARGS].",
    NULL },
  { "shell-fail", '\0',
    G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, opt_shell_cb,
    "Run an interactive shell after COMMAND, but only if it fails.",
    NULL },
  { "shell-instead", '\0',
    G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, opt_shell_cb,
    "Run an interactive shell instead of COMMAND. Executing \"$@\" in that "
    "shell will run COMMAND [ARGS].",
    NULL },
  { "terminal", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, opt_terminal_cb,
    "none: disable features that would use a terminal; "
    "auto: equivalent to xterm if a --shell option is used, or none; "
    "xterm: put game output (and --shell if used) in an xterm; "
    "tty: put game output (and --shell if used) on Steam's "
    "controlling tty "
    "[Default: $PRESSURE_VESSEL_TERMINAL or 'auto']",
    "{none|auto|xterm|tty}" },
  { "tty", '\0',
    G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, opt_terminal_cb,
    "Equivalent to --terminal=tty", NULL },
  { "xterm", '\0',
    G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, opt_terminal_cb,
    "Equivalent to --terminal=xterm", NULL },
  { "terminate-idle-timeout", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_DOUBLE, &opt_terminate_idle_timeout,
    "If --terminate-timeout is used, wait this many seconds before "
    "sending SIGTERM. [default: 0.0]",
    "SECONDS" },
  { "terminate-timeout", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_DOUBLE, &opt_terminate_timeout,
    "Send SIGTERM and SIGCONT to descendant processes that didn't "
    "exit within --terminate-idle-timeout. If they don't all exit within "
    "this many seconds, send SIGKILL and SIGCONT to survivors. If 0.0, "
    "skip SIGTERM and use SIGKILL immediately. Implies --subreaper. "
    "[Default: -1.0, meaning don't signal].",
    "SECONDS" },
  { "verbose", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_verbose,
    "Be more verbose.", NULL },
  { "version", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_version,
    "Print version number and exit.", NULL },
  { "version-only", '\0',
    G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_version_only,
    "Print version number (no other information) and exit.", NULL },
  { "with-host-graphics", '\0',
    G_OPTION_FLAG_NO_ARG | G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_CALLBACK,
    opt_with_host_graphics_cb,
    "Deprecated alias for \"--graphics-provider=/\" or "
    "\"--graphics-provider=/run/host\"", NULL },
  { "without-host-graphics", '\0',
    G_OPTION_FLAG_NO_ARG | G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_CALLBACK,
    opt_with_host_graphics_cb,
    "Deprecated alias for \"--graphics-provider=\"", NULL },
  { "write-bwrap-arguments", '\0',
    G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_FILENAME, &opt_write_bwrap,
    "Write the final bwrap arguments, as null terminated strings, to the "
    "given file path.", "PATH" },
  { "test", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_test,
    "Smoke test pressure-vessel-wrap and exit.", NULL },
  { "only-prepare", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_only_prepare,
    "Prepare runtime, but do not actually run anything.", NULL },
  { NULL }
};

static Tristate
tristate_environment (const gchar *name)
{
  const gchar *value = g_getenv (name);

  if (g_strcmp0 (value, "1") == 0)
    return TRISTATE_YES;

  if (g_strcmp0 (value, "0") == 0)
    return TRISTATE_NO;

  if (value != NULL && value[0] != '\0')
    g_warning ("Unrecognised value \"%s\" for $%s", value, name);

  return TRISTATE_MAYBE;
}

static int my_pid = -1;

static void
cli_log_func (const gchar *log_domain,
              GLogLevelFlags log_level,
              const gchar *message,
              gpointer user_data)
{
  g_printerr ("%s[%d]: %s\n", (const char *) user_data, my_pid, message);
}

int
main (int argc,
      char *argv[])
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  int ret = 2;
  gsize i;
  g_auto(GStrv) original_argv = NULL;
  int original_argc = argc;
  gboolean is_flatpak_env = g_file_test ("/.flatpak-info", G_FILE_TEST_IS_REGULAR);
  g_autoptr(FlatpakBwrap) bwrap = NULL;
  g_autoptr(FlatpakBwrap) exports_bwrap = NULL;
  g_autoptr(FlatpakExports) exports = NULL;
  g_autoptr(FlatpakBwrap) adverb_args = NULL;
  g_autofree gchar *adverb_in_container = NULL;
  g_autoptr(FlatpakBwrap) wrapped_command = NULL;
  g_autofree gchar *spawn_executable = NULL;
  g_autofree gchar *bwrap_executable = NULL;
  g_autoptr(GString) adjusted_ld_preload = g_string_new ("");
  g_autofree gchar *cwd_p = NULL;
  g_autofree gchar *cwd_l = NULL;
  g_autofree gchar *cwd_p_host = NULL;
  const gchar *home;
  g_autofree gchar *bwrap_help = NULL;
  g_autofree gchar *tools_dir = NULL;
  const gchar *bwrap_help_argv[] = { "<bwrap>", "--help", NULL };
  g_autoptr(PvRuntime) runtime = NULL;
  g_autoptr(FILE) original_stdout = NULL;

  my_pid = getpid ();

  setlocale (LC_ALL, "");

  g_set_prgname ("pressure-vessel-wrap");

  g_log_set_handler (G_LOG_DOMAIN,
                     G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE,
                     cli_log_func, (void *) g_get_prgname ());

  original_argv = g_new0 (char *, argc + 1);

  for (i = 0; i < argc; i++)
    original_argv[i] = g_strdup (argv[i]);

  if (g_getenv ("STEAM_RUNTIME") != NULL)
    {
      g_printerr ("%s: This program should not be run in the Steam Runtime.",
                  g_get_prgname ());
      g_printerr ("%s: Use pressure-vessel-unruntime instead.",
                  g_get_prgname ());
      ret = 2;
      goto out;
    }

  /* Set defaults */
  opt_batch = pv_boolean_environment ("PRESSURE_VESSEL_BATCH", FALSE);

  opt_freedesktop_app_id = g_strdup (g_getenv ("PRESSURE_VESSEL_FDO_APP_ID"));

  if (opt_freedesktop_app_id != NULL && opt_freedesktop_app_id[0] == '\0')
    g_clear_pointer (&opt_freedesktop_app_id, g_free);

  opt_home = g_strdup (g_getenv ("PRESSURE_VESSEL_HOME"));

  if (opt_home != NULL && opt_home[0] == '\0')
    g_clear_pointer (&opt_home, g_free);

  opt_remove_game_overlay = pv_boolean_environment ("PRESSURE_VESSEL_REMOVE_GAME_OVERLAY",
                                                    FALSE);
  opt_share_home = tristate_environment ("PRESSURE_VESSEL_SHARE_HOME");
  opt_gc_runtimes = pv_boolean_environment ("PRESSURE_VESSEL_GC_RUNTIMES", TRUE);
  opt_generate_locales = pv_boolean_environment ("PRESSURE_VESSEL_GENERATE_LOCALES", TRUE);

  opt_share_pid = pv_boolean_environment ("PRESSURE_VESSEL_SHARE_PID", TRUE);
  opt_verbose = pv_boolean_environment ("PRESSURE_VESSEL_VERBOSE", FALSE);

  if (!opt_shell_cb ("$PRESSURE_VESSEL_SHELL",
                     g_getenv ("PRESSURE_VESSEL_SHELL"), NULL, error))
    goto out;

  if (!opt_terminal_cb ("$PRESSURE_VESSEL_TERMINAL",
                        g_getenv ("PRESSURE_VESSEL_TERMINAL"), NULL, error))
    goto out;

  context = g_option_context_new ("[--] COMMAND [ARGS]\n"
                                  "Run COMMAND [ARGS] in a container.\n");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (opt_runtime == NULL)
    opt_runtime = g_strdup (g_getenv ("PRESSURE_VESSEL_RUNTIME"));

  if (opt_runtime_base == NULL)
    opt_runtime_base = g_strdup (g_getenv ("PRESSURE_VESSEL_RUNTIME_BASE"));

  if (opt_runtime != NULL
      && opt_runtime[0] != '\0'
      && !g_path_is_absolute (opt_runtime)
      && opt_runtime_base != NULL
      && opt_runtime_base[0] != '\0')
    {
      g_autofree gchar *tmp = g_steal_pointer (&opt_runtime);

      opt_runtime = g_build_filename (opt_runtime_base, tmp, NULL);
    }

  if (opt_copy_runtime_into == NULL)
    opt_copy_runtime_into = g_strdup (g_getenv ("PRESSURE_VESSEL_COPY_RUNTIME_INTO"));

  if (opt_copy_runtime_into != NULL
      && opt_copy_runtime_into[0] == '\0')
    opt_copy_runtime_into = NULL;

  if (opt_graphics_provider == NULL)
    opt_graphics_provider = g_strdup (g_getenv ("PRESSURE_VESSEL_GRAPHICS_PROVIDER"));

  if (opt_graphics_provider == NULL)
    {
      /* Also check the deprecated 'PRESSURE_VESSEL_HOST_GRAPHICS' */
      if (!pv_boolean_environment ("PRESSURE_VESSEL_HOST_GRAPHICS", TRUE))
        opt_graphics_provider = g_strdup ("");
      else if (g_file_test ("/run/host", G_FILE_TEST_IS_DIR))
        opt_graphics_provider = g_strdup ("/run/host");
      else
        opt_graphics_provider = g_strdup ("/");
    }

  g_assert (opt_graphics_provider != NULL);
  if (opt_graphics_provider[0] != '\0' && opt_graphics_provider[0] != '/')
    {
      g_printerr ("%s: --graphics-provider path must be absolute, not \"%s\"\n",
                  g_get_prgname (), opt_graphics_provider);
      goto out;
    }

  if (g_strcmp0 (opt_graphics_provider, "/") == 0)
    graphics_provider_mount_point = g_strdup ("/run/host");
  else
    graphics_provider_mount_point = g_strdup ("/run/gfx");

  if (opt_version_only)
    {
      g_print ("%s\n", VERSION);
      ret = 0;
      goto out;
    }

  if (opt_version)
    {
      g_print ("%s:\n"
               " Package: pressure-vessel\n"
               " Version: %s\n",
               argv[0], VERSION);
      ret = 0;
      goto out;
    }

  original_stdout = pv_divert_stdout_to_stderr (error);

  if (original_stdout == NULL)
    {
      ret = 1;
      goto out;
    }

  pv_avoid_gvfs ();

  if (argc < 2 && !opt_test && !opt_only_prepare)
    {
      g_printerr ("%s: An executable to run is required\n",
                  g_get_prgname ());
      goto out;
    }

  if (opt_terminal == PV_TERMINAL_AUTO)
    {
      if (opt_shell != PV_SHELL_NONE)
        opt_terminal = PV_TERMINAL_XTERM;
      else
        opt_terminal = PV_TERMINAL_NONE;
    }

  if (opt_terminal == PV_TERMINAL_NONE && opt_shell != PV_SHELL_NONE)
    {
      g_printerr ("%s: --terminal=none is incompatible with --shell\n",
                  g_get_prgname ());
      goto out;
    }

  if (opt_batch)
    {
      /* --batch or PRESSURE_VESSEL_BATCH=1 overrides these */
      opt_shell = PV_SHELL_NONE;
      opt_terminal = PV_TERMINAL_NONE;
    }

  if (argc > 1 && strcmp (argv[1], "--") == 0)
    {
      argv++;
      argc--;
    }

  home = g_get_home_dir ();

  if (opt_share_home == TRISTATE_YES)
    {
      opt_fake_home = NULL;
    }
  else if (opt_home)
    {
      opt_fake_home = g_strdup (opt_home);
    }
  else if (opt_share_home == TRISTATE_MAYBE)
    {
      opt_fake_home = NULL;
    }
  else if (opt_freedesktop_app_id)
    {
      opt_fake_home = g_build_filename (home, ".var", "app",
                                        opt_freedesktop_app_id, NULL);
    }
  else if (opt_steam_app_id)
    {
      opt_freedesktop_app_id = g_strdup_printf ("com.steampowered.App%s",
                                                opt_steam_app_id);
      opt_fake_home = g_build_filename (home, ".var", "app",
                                        opt_freedesktop_app_id, NULL);
    }
  else if (g_getenv ("STEAM_COMPAT_APP_ID") != NULL)
    {
      opt_freedesktop_app_id = g_strdup_printf ("com.steampowered.App%s",
                                                g_getenv ("STEAM_COMPAT_APP_ID"));
      opt_fake_home = g_build_filename (home, ".var", "app",
                                        opt_freedesktop_app_id, NULL);
    }
  else if (g_getenv ("SteamAppId") != NULL)
    {
      opt_freedesktop_app_id = g_strdup_printf ("com.steampowered.App%s",
                                                g_getenv ("SteamAppId"));
      opt_fake_home = g_build_filename (home, ".var", "app",
                                        opt_freedesktop_app_id, NULL);
    }
  else
    {
      g_printerr ("%s: Either --home, --freedesktop-app-id, --steam-app-id "
                  "or $SteamAppId is required\n",
                  g_get_prgname ());
      goto out;
    }

  if (opt_env_if_host != NULL)
    {
      for (i = 0; opt_env_if_host[i] != NULL; i++)
        {
          const char *equals = strchr (opt_env_if_host[i], '=');

          if (equals == NULL)
            {
              g_printerr ("%s: --env-if-host argument must be of the form "
                          "NAME=VALUE, not \"%s\"\n",
                          g_get_prgname (), opt_env_if_host[i]);
              goto out;
            }
        }
    }

  if (opt_only_prepare && opt_test)
    {
      g_printerr ("%s: --only-prepare and --test are mutually exclusive",
                  g_get_prgname ());
      goto out;
    }

  if (opt_filesystems != NULL)
    {
      for (i = 0; opt_filesystems[i] != NULL; i++)
        {
          if (strchr (opt_filesystems[i], ':') != NULL ||
              strchr (opt_filesystems[i], '\\') != NULL)
            {
              g_printerr ("%s: ':' and '\\' in --filesystem argument "
                          "not handled yet\n",
                          g_get_prgname ());
              goto out;
            }
          else if (!g_path_is_absolute (opt_filesystems[i]))
            {
              g_printerr ("%s: --filesystem argument must be an absolute "
                          "path, not \"%s\"\n",
                          g_get_prgname (), opt_filesystems[i]);
              goto out;
            }
        }
    }

  /* Finished parsing arguments, so any subsequent failures will make
   * us exit 1. */
  ret = 1;

  if (opt_terminal != PV_TERMINAL_TTY)
    {
      int fd;

      if (!glnx_openat_rdonly (-1, "/dev/null", TRUE, &fd, error))
          goto out;

      if (dup2 (fd, STDIN_FILENO) < 0)
        {
          glnx_throw_errno_prefix (error,
                                   "Cannot replace stdin with /dev/null");
          goto out;
        }
    }

  pv_get_current_dirs (&cwd_p, &cwd_l);

  if (opt_verbose)
    {
      g_auto(GStrv) env = g_get_environ ();

      g_log_set_handler (G_LOG_DOMAIN,
                         G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO,
                         cli_log_func, (void *) g_get_prgname ());

      g_message ("Original argv:");

      for (i = 0; i < original_argc; i++)
        {
          g_autofree gchar *quoted = g_shell_quote (original_argv[i]);

          g_message ("\t%" G_GSIZE_FORMAT ": %s", i, quoted);
        }

      g_message ("Current working directory:");
      g_message ("\tPhysical: %s", cwd_p);
      g_message ("\tLogical: %s", cwd_l);

      g_message ("Environment variables:");

      qsort (env, g_strv_length (env), sizeof (char *), pv_envp_cmp);

      for (i = 0; env[i] != NULL; i++)
        {
          g_autofree gchar *quoted = g_shell_quote (env[i]);

          g_message ("\t%s", quoted);
        }

      g_message ("Wrapped command:");

      for (i = 1; i < argc; i++)
        {
          g_autofree gchar *quoted = g_shell_quote (argv[i]);

          g_message ("\t%" G_GSIZE_FORMAT ": %s", i, quoted);
        }
    }

  tools_dir = find_executable_dir (error);

  if (tools_dir == NULL)
    goto out;

  g_debug ("Found executable directory: %s", tools_dir);

  wrapped_command = flatpak_bwrap_new (flatpak_bwrap_empty_env);

  if (argc > 1 && argv[1][0] == '-')
    {
      /* Make sure wrapped_command is something we can validly pass to env(1) */
      if (strchr (argv[1], '=') != NULL)
        flatpak_bwrap_add_args (wrapped_command,
                                "sh", "-euc", "exec \"$@\"", "sh",
                                NULL);

      /* Make sure bwrap will interpret wrapped_command as the end of its
       * options */
      flatpak_bwrap_add_arg (wrapped_command, "env");
    }

  g_debug ("Setting arguments for wrapped command");
  flatpak_bwrap_append_argsv (wrapped_command, &argv[1], argc - 1);

  /* If we are in a Flatpak environment we can't use bwrap directly */
  if (is_flatpak_env)
    {
      g_debug ("Checking for flatpak-spawn...");
      spawn_executable = check_flatpak_spawn ();
      /* Assume "bwrap" to exist in the host system and to be in its PATH */
      bwrap_executable = g_strdup ("bwrap");
    }
  else
    {
      g_debug ("Checking for bwrap...");
      bwrap_executable = check_bwrap (tools_dir, opt_only_prepare);
    }

  if (opt_test)
    {
      if ((is_flatpak_env && spawn_executable == NULL)
          || bwrap_executable == NULL)
        {
          ret = 1;
          goto out;
        }
      else
        {
          if (spawn_executable != NULL)
            g_debug ("OK (%s) (%s)", spawn_executable, bwrap_executable);
          else
            g_debug ("OK (%s)", bwrap_executable);
          ret = 0;
          goto out;
        }
    }

  if ((is_flatpak_env && spawn_executable == NULL)
      || bwrap_executable == NULL)
    {
      /* TODO in a Flatpak environment, host is not what we expect it to be */
      if (opt_host_fallback)
        {
          g_message ("Falling back to executing wrapped command directly");

          if (opt_env_if_host != NULL)
            {
              for (i = 0; opt_env_if_host[i] != NULL; i++)
                {
                  char *equals = strchr (opt_env_if_host[i], '=');

                  g_assert (equals != NULL);

                  *equals = '\0';
                  flatpak_bwrap_set_env (wrapped_command, opt_env_if_host[i],
                                         equals + 1, TRUE);
                }
            }

          flatpak_bwrap_finish (wrapped_command);

          /* flatpak_bwrap_finish did this */
          g_assert (g_ptr_array_index (wrapped_command->argv,
                                       wrapped_command->argv->len - 1) == NULL);

          execvpe (g_ptr_array_index (wrapped_command->argv, 0),
                   (char * const *) wrapped_command->argv->pdata,
                   wrapped_command->envp);

          glnx_throw_errno_prefix (error, "execvpe %s",
                                   (gchar *) g_ptr_array_index (wrapped_command->argv, 0));
          goto out;
        }
      else
        {
          goto out;
        }
    }

  if (!is_flatpak_env)
    {
      g_debug ("Checking bwrap features...");
      bwrap_help_argv[0] = bwrap_executable;
      bwrap_help = pv_capture_output (bwrap_help_argv, error);

      if (bwrap_help == NULL)
        goto out;
    }

  bwrap = flatpak_bwrap_new (NULL);
  flatpak_bwrap_add_arg (bwrap, bwrap_executable);
  exports = flatpak_exports_new ();

  if (is_flatpak_env)
    {
      glnx_autofd int fd = TEMP_FAILURE_RETRY (open ("/run/host",
                                                     O_CLOEXEC | O_PATH));

      if (fd < 0)
        {
          glnx_throw_errno_prefix (error, "Unable to open /run/host");
          goto out;
        }

      flatpak_exports_take_host_fd (exports, glnx_steal_fd (&fd));
    }

  /* Protect the controlling terminal from the app/game, unless we are
   * running an interactive shell in which case that would break its
   * job control. */
  if (opt_terminal != PV_TERMINAL_TTY)
    flatpak_bwrap_add_arg (bwrap, "--new-session");

  /* Start with just the root tmpfs (which appears automatically)
   * and the standard API filesystems */
  pv_bwrap_add_api_filesystems (bwrap);

  /* The FlatpakExports will populate /run/host for us */
  flatpak_exports_add_host_os_expose (exports,
                                      FLATPAK_FILESYSTEM_MODE_READ_ONLY);

  /* steam-runtime-system-info uses this to detect pressure-vessel, so we
   * need to create it even if it will be empty */
  flatpak_bwrap_add_args (bwrap,
                          "--dir",
                          "/run/pressure-vessel",
                          NULL);

  if (opt_runtime != NULL && opt_runtime[0] != '\0')
    {
      PvRuntimeFlags flags = PV_RUNTIME_FLAGS_NONE;

      if (opt_gc_runtimes)
        flags |= PV_RUNTIME_FLAGS_GC_RUNTIMES;

      if (opt_generate_locales)
        flags |= PV_RUNTIME_FLAGS_GENERATE_LOCALES;

      if (opt_graphics_provider != NULL && opt_graphics_provider[0] != '\0')
        flags |= PV_RUNTIME_FLAGS_PROVIDER_GRAPHICS_STACK;

      if (opt_verbose)
        flags |= PV_RUNTIME_FLAGS_VERBOSE;

      g_debug ("Configuring runtime %s...", opt_runtime);

      runtime = pv_runtime_new (opt_runtime,
                                opt_copy_runtime_into,
                                bwrap_executable,
                                tools_dir,
                                opt_graphics_provider,
                                graphics_provider_mount_point,
                                flags,
                                error);

      if (runtime == NULL)
        goto out;

      if (!pv_runtime_bind (runtime, bwrap, error))
        goto out;
    }
  else if (is_flatpak_env)
    {
      glnx_throw (error,
                  "Cannot operate without a runtime from inside a "
                  "Flatpak app");
      goto out;
    }
  else
    {
      static const char * const export_os_mutable[] = { "/etc", "/tmp", "/var" };

      g_assert (!is_flatpak_env);

      if (!pv_bwrap_bind_usr (bwrap, "/", "/", "/", error))
        goto out;

      /* This mounts over the top of the subset of /etc mounted by
       * pv_bwrap_bind_usr(), but that's harmless. */
      for (i = 0; i < G_N_ELEMENTS (export_os_mutable); i++)
        {
          const char *dir = export_os_mutable[i];

          if (g_file_test (dir, G_FILE_TEST_EXISTS))
            flatpak_bwrap_add_args (bwrap, "--bind", dir, dir, NULL);
        }

      /* We do each subdirectory of /run separately, so that we can
       * always create /run/host and /run/pressure-vessel. */
      if (!export_contents_of_run (bwrap, error))
        goto out;

      /* This handles everything except:
       *
       * /app (should be unnecessary)
       * /boot (should be unnecessary)
       * /dev (handled by pv_bwrap_add_api_filesystems() above)
       * /etc (handled by export_os_mutable)
       * /proc (handled by pv_bwrap_add_api_filesystems() above)
       * /root (should be unnecessary)
       * /run (handled by export_contents_of_run())
       * /sys (handled by pv_bwrap_add_api_filesystems() above)
       * /tmp (handled by export_os_mutable)
       * /usr, /lib, /lib32, /lib64, /bin, /sbin
       *  (all handled by pv_bwrap_bind_usr() above)
       * /var (handled by export_os_mutable)
       */
      if (!export_root_dirs_like_filesystem_host (exports,
                                                  FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                                  error))
        goto out;
    }

  /* Protect other users' homes (but guard against the unlikely
   * situation that they don't exist) */
  if (g_file_test ("/home", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap,
                            "--tmpfs", "/home",
                            NULL);

  g_debug ("Making home directory available...");

  if (opt_fake_home == NULL)
    {
      flatpak_exports_add_path_expose (exports,
                                       FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                       home);
    }
  else
    {
      if (!use_fake_home (exports, bwrap, opt_fake_home, error))
        goto out;
    }

  if (!opt_share_pid)
    {
      g_warning ("Unsharing process ID namespace. This is not expected "
                 "to work...");
      flatpak_bwrap_add_arg (bwrap, "--unshare-pid");
    }

  /* Always export /tmp for now. SteamVR uses this as a rendezvous
   * directory for IPC. */
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                   "/tmp");

  g_debug ("Adjusting LD_PRELOAD...");

  /* We need the LD_PRELOADs from Steam visible at the paths that were
   * used for them, which might be their physical rather than logical
   * locations. */
  if (opt_ld_preload != NULL)
    {
      for (i = 0; i < opt_ld_preload->len; i++)
        {
          const char *preload = g_ptr_array_index (opt_ld_preload, i);

          g_assert (preload != NULL);

          if (*preload == '\0')
            continue;

          /* We have the beginnings of infrastructure to set a LD_PRELOAD
           * from inside the container, but currently the only thing we
           * support is it coming from the host. */
          g_assert (g_str_has_prefix (preload, "host:"));
          preload = preload + 5;

          if (g_file_test (preload, G_FILE_TEST_EXISTS))
            {
              if (opt_remove_game_overlay
                  && g_str_has_suffix (preload, "/gameoverlayrenderer.so"))
                {
                  g_debug ("Disabling Steam Overlay: %s", preload);
                  continue;
                }

              if (runtime != NULL
                  && (g_str_has_prefix (preload, "/usr/")
                      || g_str_has_prefix (preload, "/lib")))
                {
                  g_autofree gchar *in_run_host = g_build_filename ("/run/host",
                                                                    preload,
                                                                    NULL);

                  /* When using a runtime we can't write to /usr/ or /libQUAL/,
                   * so redirect this preloaded module to the corresponding
                   * location in /run/host. */
                  pv_search_path_append (adjusted_ld_preload, in_run_host);
                }
              else
                {
                  flatpak_exports_add_path_expose (exports,
                                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                                   preload);
                  pv_search_path_append (adjusted_ld_preload, preload);
                }
            }
          else
            {
              g_debug ("LD_PRELOAD module '%s' does not exist", preload);
            }
        }
    }

  /* Put the caller's LD_PRELOAD back.
   * This would be filtered out by a setuid bwrap, so we have to go
   * via --setenv. */

  if (adjusted_ld_preload->len != 0)
      flatpak_bwrap_add_args (bwrap,
                              "--setenv", "LD_PRELOAD",
                              adjusted_ld_preload->str,
                              NULL);
  else
      flatpak_bwrap_add_args (bwrap,
                              "--unsetenv", "LD_PRELOAD",
                              NULL);

  g_debug ("Making Steam environment variables available if required...");
  for (i = 0; i < G_N_ELEMENTS (known_required_env); i++)
    bind_and_propagate_from_environ (exports, bwrap,
                                     FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                     known_required_env[i].name,
                                     known_required_env[i].flags);

  /* Make arbitrary filesystems available. This is not as complete as
   * Flatpak yet. */
  if (opt_filesystems != NULL)
    {
      g_debug ("Processing --filesystem arguments...");

      for (i = 0; opt_filesystems[i] != NULL; i++)
        {
          /* We already checked this */
          g_assert (g_path_is_absolute (opt_filesystems[i]));

          g_debug ("Bind-mounting \"%s\"", opt_filesystems[i]);
          flatpak_exports_add_path_expose (exports,
                                           FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                           opt_filesystems[i]);
        }
    }

  /* Make sure the current working directory (the game we are going to
   * run) is available. Some games write here. */
  g_debug ("Making current working directory available...");

  cwd_p_host = pv_current_namespace_path_to_host_path (cwd_p);

  if (pv_is_same_file (home, cwd_p))
    {
      g_debug ("Not making physical working directory \"%s\" available to "
               "container because it is the home directory",
               cwd_p);
    }
  else
    {
      /* If in Flatpak, we assume that cwd_p_host is visible in the
       * current namespace as well as in the host, because it's
       * either in our ~/.var/app/$FLATPAK_ID, or a --filesystem that
       * was exposed from the host. */
      flatpak_exports_add_path_expose (exports,
                                       FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                       cwd_p_host);
    }

  flatpak_bwrap_add_args (bwrap,
                          "--chdir", cwd_p_host,
                          "--unsetenv", "PWD",
                          NULL);

  /* Put Steam Runtime environment variables back, if /usr is mounted
   * from the host. */
  if (runtime == NULL)
    {
      g_debug ("Making Steam Runtime available...");

      /* We need libraries from the Steam Runtime, so make sure that's
       * visible (it should never need to be read/write though) */
      if (opt_env_if_host != NULL)
        {
          for (i = 0; opt_env_if_host[i] != NULL; i++)
            {
              char *equals = strchr (opt_env_if_host[i], '=');

              g_assert (equals != NULL);

              if (g_str_has_prefix (opt_env_if_host[i], "STEAM_RUNTIME=/"))
                flatpak_exports_add_path_expose (exports,
                                                 FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                                 equals + 1);

              *equals = '\0';
              /* We do this via --setenv instead of flatpak_bwrap_set_env()
               * to make sure they aren't filtered out by a setuid bwrap. */
              flatpak_bwrap_add_args (bwrap,
                                      "--setenv", opt_env_if_host[i],
                                      equals + 1,
                                      NULL);
              *equals = '=';
            }
        }
    }

  /* Convert the exported directories into extra bubblewrap arguments */
  exports_bwrap = flatpak_bwrap_new (flatpak_bwrap_empty_env);
  flatpak_exports_append_bwrap_args (exports, exports_bwrap);
  adjust_exports (exports_bwrap, home);
  flatpak_bwrap_append_bwrap (bwrap, exports_bwrap);

  /* We need to set up IPC rendezvous points relatively late, so that
   * even if we are sharing /tmp via --filesystem=/tmp, we'll still
   * mount our own /tmp/.X11-unix over the top of the OS's. */
  if (runtime != NULL)
    {
      flatpak_run_add_wayland_args (bwrap);

      /* When in a Flatpak container the "DISPLAY" env is equal to ":99.0",
       * but it might be different on the host system. As a workaround we simply
       * bind the whole "/tmp/.X11-unix" directory and unset the container
       * "DISPLAY" env.
       */
      if (g_file_test ("/.flatpak-info", G_FILE_TEST_IS_REGULAR))
        {
          flatpak_bwrap_add_args (bwrap,
                                  "--ro-bind", "/tmp/.X11-unix", "/tmp/.X11-unix",
                                  NULL);
          flatpak_bwrap_unset_env (bwrap, "DISPLAY");
        }
      else
        {
          flatpak_run_add_x11_args (bwrap, TRUE);
        }

      flatpak_run_add_pulseaudio_args (bwrap);
      flatpak_run_add_session_dbus_args (bwrap);
      flatpak_run_add_system_dbus_args (bwrap);
    }

  if (opt_verbose)
    {
      g_message ("%s options before bundling:", bwrap_executable);

      for (i = 0; i < bwrap->argv->len; i++)
        {
          g_autofree gchar *quoted = NULL;

          quoted = g_shell_quote (g_ptr_array_index (bwrap->argv, i));
          g_message ("\t%s", quoted);
        }
    }

  if (!opt_only_prepare)
    {
      if (!flatpak_bwrap_bundle_args (bwrap, 1, -1, FALSE, error))
        goto out;
    }

  adverb_args = flatpak_bwrap_new (flatpak_bwrap_empty_env);

  if (runtime != NULL)
    adverb_in_container = pv_runtime_get_adverb (runtime, adverb_args);

  if (opt_terminate_timeout >= 0.0)
    {
      if (opt_terminate_idle_timeout > 0.0)
        flatpak_bwrap_add_arg_printf (adverb_args,
                                      "--terminate-idle-timeout=%f",
                                      opt_terminate_idle_timeout);

      flatpak_bwrap_add_arg_printf (adverb_args,
                                    "--terminate-timeout=%f",
                                    opt_terminate_timeout);
    }

  /* If not using a runtime, the adverb in the container has the
   * same path as outside */
  if (adverb_in_container == NULL)
    adverb_in_container = g_build_filename (tools_dir,
                                            "pressure-vessel-adverb",
                                            NULL);

  flatpak_bwrap_add_args (bwrap,
                          adverb_in_container,
                          "--exit-with-parent",
                          "--subreaper",
                          NULL);

  if (opt_pass_fds != NULL)
    {
      for (i = 0; i < opt_pass_fds->len; i++)
        {
          int fd = g_array_index (opt_pass_fds, int, i);

          flatpak_bwrap_add_fd (bwrap, fd);
          flatpak_bwrap_add_arg_printf (bwrap, "--pass-fd=%d", fd);
        }
    }

  switch (opt_shell)
    {
      case PV_SHELL_AFTER:
        flatpak_bwrap_add_arg (bwrap, "--shell=after");
        break;

      case PV_SHELL_FAIL:
        flatpak_bwrap_add_arg (bwrap, "--shell=fail");
        break;

      case PV_SHELL_INSTEAD:
        flatpak_bwrap_add_arg (bwrap, "--shell=instead");
        break;

      case PV_SHELL_NONE:
        flatpak_bwrap_add_arg (bwrap, "--shell=none");
        break;

      default:
        g_warn_if_reached ();
    }

  switch (opt_terminal)
    {
      case PV_TERMINAL_AUTO:
        flatpak_bwrap_add_arg (bwrap, "--terminal=auto");
        break;

      case PV_TERMINAL_NONE:
        flatpak_bwrap_add_arg (bwrap, "--terminal=none");
        break;

      case PV_TERMINAL_TTY:
        flatpak_bwrap_add_arg (bwrap, "--terminal=tty");
        break;

      case PV_TERMINAL_XTERM:
        flatpak_bwrap_add_arg (bwrap, "--terminal=xterm");
        break;

      default:
        g_warn_if_reached ();
        break;
    }

  if (opt_verbose)
    flatpak_bwrap_add_arg (bwrap, "--verbose");

  flatpak_bwrap_append_bwrap (bwrap, adverb_args);
  flatpak_bwrap_add_arg (bwrap, "--");

  g_debug ("Adding wrapped command...");
  flatpak_bwrap_append_args (bwrap, wrapped_command->argv);

  if (is_flatpak_env)
    {
      /* Just use the envp from @bwrap */
      g_autoptr(FlatpakBwrap) flatpak_spawn = flatpak_bwrap_new (flatpak_bwrap_empty_env);
      flatpak_bwrap_add_arg (flatpak_spawn, spawn_executable);
      flatpak_bwrap_add_arg (flatpak_spawn, "--host");

      for (i = 0; i < bwrap->fds->len; i++)
        {
          g_autofree char *fd_str = g_strdup_printf ("--forward-fd=%d",
                                                     g_array_index (bwrap->fds, int, i));
          flatpak_bwrap_add_arg (flatpak_spawn, fd_str);
        }
      /* Change the current working directory where flatpak-spawn will run.
       * Bwrap will then set its directory by itself. For this reason here
       * we just need a directory that it's known to exist. */
      flatpak_bwrap_add_arg (flatpak_spawn, "--directory=/");

      flatpak_bwrap_append_bwrap (flatpak_spawn, bwrap);
      g_clear_pointer (&bwrap, flatpak_bwrap_free);
      bwrap = g_steal_pointer (&flatpak_spawn);
    }

  if (opt_verbose)
    {
      g_message ("Final %s options:", bwrap_executable);

      for (i = 0; i < bwrap->argv->len; i++)
        {
          g_autofree gchar *quoted = NULL;

          quoted = g_shell_quote (g_ptr_array_index (bwrap->argv, i));
          g_message ("\t%s", quoted);
        }

      g_message ("%s environment:", bwrap_executable);

      for (i = 0; bwrap->envp != NULL && bwrap->envp[i] != NULL; i++)
        {
          g_autofree gchar *quoted = NULL;

          quoted = g_shell_quote (bwrap->envp[i]);
          g_message ("\t%s", quoted);
        }
    }

  /* Clean up temporary directory before running our long-running process */
  if (runtime != NULL)
    pv_runtime_cleanup (runtime);

  flatpak_bwrap_finish (bwrap);

  if (opt_write_bwrap != NULL)
    {
      FILE *file = fopen (opt_write_bwrap, "w");
      if (file == NULL)
        {
          g_warning ("An error occurred trying to write the bwrap arguments: %s",
                    g_strerror (errno));
          /* This is not a fatal error, try to continue */
        }
      else
        {
          for (i = 0; i < bwrap->argv->len; i++)
            fprintf (file, "%s%c", (gchar *)g_ptr_array_index (bwrap->argv, i), '\0');

          fclose (file);
        }
    }

  if (opt_only_prepare)
    ret = 0;
  else
    pv_bwrap_execve (bwrap, fileno (original_stdout), error);

out:
  if (local_error != NULL)
    g_warning ("%s", local_error->message);

  g_clear_pointer (&opt_ld_preload, g_ptr_array_unref);
  g_clear_pointer (&opt_env_if_host, g_strfreev);
  g_clear_pointer (&opt_freedesktop_app_id, g_free);
  g_clear_pointer (&opt_steam_app_id, g_free);
  g_clear_pointer (&opt_home, g_free);
  g_clear_pointer (&opt_fake_home, g_free);
  g_clear_pointer (&opt_runtime_base, g_free);
  g_clear_pointer (&opt_runtime, g_free);
  g_clear_pointer (&opt_pass_fds, g_array_unref);

  g_debug ("Exiting with status %d", ret);
  return ret;
}