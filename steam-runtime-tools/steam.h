/*
 * Copyright © 2019 Collabora Ltd.
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

#if !defined(_SRT_IN_SINGLE_HEADER) && !defined(_SRT_COMPILATION)
#error "Do not include directly, use <steam-runtime-tools/steam-runtime-tools.h>"
#endif

/**
 * SrtSteamIssues:
 * @SRT_STEAM_ISSUES_NONE: There are no problems
 * @SRT_STEAM_ISSUES_INTERNAL_ERROR: Unable to detect the status of the
 *  Steam installation
 * @SRT_STEAM_ISSUES_CANNOT_FIND: Unable to find the Steam installation
 * @SRT_STEAM_ISSUES_DOT_STEAM_STEAM_NOT_SYMLINK: `~/.steam/steam` is not
 *  a symbolic link to a Steam installation, which for example can happen
 *  if Steam was installed on a system with https://bugs.debian.org/916303
 *
 * A bitfield with flags representing problems with the Steam
 * installation, or %SRT_STEAM_ISSUES_NONE (which is numerically zero)
 * if no problems were detected.
 *
 * In general, more bits set means more problems.
 */
typedef enum
{
  SRT_STEAM_ISSUES_INTERNAL_ERROR = (1 << 0),
  SRT_STEAM_ISSUES_CANNOT_FIND = (1 << 1),
  SRT_STEAM_ISSUES_DOT_STEAM_STEAM_NOT_SYMLINK = (1 << 2),
  SRT_STEAM_ISSUES_NONE = 0
} SrtSteamIssues;