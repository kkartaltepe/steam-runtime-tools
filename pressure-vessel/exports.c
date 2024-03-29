/*
 * Copyright © 2017-2020 Collabora Ltd.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "exports.h"

#include <ftw.h>

#include <steam-runtime-tools/utils-internal.h>

/* nftw() doesn't have a user_data argument so we need to use a global
 * variable :-( */
static struct
{
  FlatpakExports *exports;
  const char *log_replace_prefix;
  const char *log_replace_with;
} export_targets_data;

/* If a symlink target matches one of these prefixes, it's assumed to be
 * intended to refer to a path inside the container, not a path on the host,
 * and therefore not exported.
 *
 * In fact we wouldn't export most of these anyway, because FlatpakExports
 * specifically excludes them - but it's confusing to get log messages saying
 * "Exporting foo because bar", "Unable to open path" for something that we
 * have no intention of exporting anyway. */
static const char * const exclude_prefixes[] =
{
  "/app/",
  "/bin/",
  "/dev/",
  "/etc/",
  "/lib", /* intentionally no trailing "/" to match lib64, etc. */
  "/overrides/",
  "/proc/",
  "/run/gfx/",
  "/run/host/",
  "/sbin/",
  "/usr/",
};

static int
export_targets_helper (const char *fpath,
                       const struct stat *sb,
                       int typeflag,
                       struct FTW *ftwbuf)
{
  g_autofree gchar *target = NULL;
  const char *after = NULL;
  size_t i;

  switch (typeflag)
    {
      case FTW_SL:
        target = glnx_readlinkat_malloc (-1, fpath, NULL, NULL);

        if (target[0] != '/')
          break;

        after = _srt_get_path_after (fpath, export_targets_data.log_replace_prefix);

        for (i = 0; i < G_N_ELEMENTS (exclude_prefixes); i++)
          {
            if (g_str_has_prefix (target, exclude_prefixes[i]))
              {
                if (after == NULL)
                  g_debug ("%s points to container-side path %s", fpath, target);
                else
                  g_debug ("%s/%s points to container-side path %s",
                           export_targets_data.log_replace_with, after, target);
                return 0;
              }
          }

        if (after == NULL)
          g_debug ("Exporting %s because %s points to it", target, fpath);
        else
          g_debug ("Exporting %s because %s/%s points to it",
                   target, export_targets_data.log_replace_with, after);

        flatpak_exports_add_path_expose (export_targets_data.exports,
                                         FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                         target);
        break;

      default:
        break;
    }

  return 0;
}

/**
 * pv_export_symlink_targets:
 * @exports: The #FlatpakExports
 * @source: A copy of the overrides directory, for example
 *  `/tmp/tmp12345678/overrides`.
 * @log_as: Replace the @source with @log_as in debug messages,
 *  for example `${overrides}`.
 *
 * For every symbolic link in @source, if the target is absolute, mark
 * it to be exported in @exports.
 */
void
pv_export_symlink_targets (FlatpakExports *exports,
                           const char *source,
                           const char *log_as)
{
  g_return_if_fail (export_targets_data.exports == NULL);

  export_targets_data.exports = exports;
  export_targets_data.log_replace_prefix = source;
  export_targets_data.log_replace_with = log_as;
  nftw (source, export_targets_helper, 100, FTW_PHYS);
  export_targets_data.exports = NULL;
  export_targets_data.log_replace_prefix = NULL;
  export_targets_data.log_replace_with = NULL;
}
