/*
 * Copyright © 2019-2023 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "libglnx.h"

#include <glib.h>

#include "flatpak-bwrap-private.h"
#include "per-arch-dirs.h"

typedef enum
{
  PV_PRELOAD_VARIABLE_INDEX_LD_AUDIT,
  PV_PRELOAD_VARIABLE_INDEX_LD_PRELOAD,
} PvPreloadVariableIndex;

#define PV_UNSPECIFIED_ABI (G_MAXSIZE)

typedef struct
{
  char *name;
  gsize index_in_preload_variables;
  /* An index in pv_multiarch_details, or PV_UNSPECIFIED_ABI if unspecified */
  gsize abi_index;
} PvAdverbPreloadModule;

static inline void
pv_adverb_preload_module_clear (gpointer p)
{
  PvAdverbPreloadModule *self = p;

  g_free (self->name);
}

gboolean pv_adverb_set_up_preload_modules (FlatpakBwrap *wrapped_command,
                                           PvPerArchDirs *lib_temp_dirs,
                                           const PvAdverbPreloadModule *preload_modules,
                                           gsize n_preload_modules,
                                           GError **error);
