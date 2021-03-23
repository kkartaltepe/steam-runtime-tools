/*<private_header>*/
/*
 * Copyright © 2019-2020 Collabora Ltd.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <stdio.h>

#include <glib.h>

#include <json-glib/json-glib.h>

#include <steam-runtime-tools/macros.h>

_SRT_PRIVATE_EXPORT
guint srt_get_flags_from_json_array (GType flags_type,
                                     JsonObject *json_obj,
                                     const gchar *array_member,
                                     guint flag_if_unknown);

gchar ** _srt_json_object_dup_strv_member (JsonObject *json_obj,
                                           const gchar *array_member,
                                           const gchar *placeholder);

gchar *_srt_json_object_dup_array_of_lines_member (JsonObject *json_obj,
                                                   const gchar *array_member);
void   _srt_json_builder_add_array_of_lines       (JsonBuilder *builder,
                                                   const char *name,
                                                   const char *value);

void _srt_json_builder_add_strv_value (JsonBuilder *builder,
                                       const gchar *array_name,
                                       const gchar * const *values,
                                       gboolean allow_empty_array);

void _srt_json_builder_add_error_members (JsonBuilder *builder,
                                          const GError *error);

void _srt_json_builder_add_string_force_utf8 (JsonBuilder *builder,
                                              const char *key,
                                              const char *value);
