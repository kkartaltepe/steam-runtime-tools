# Copyright 2021 Collabora Ltd.
# SPDX-License-Identifier: MIT

srt_helpers += shared_module(
  'identify-platform',
  '../identify-platform.c',
  c_args : ['-D_SRT_PLATFORM_VALUE="i486"',],
  install : true,
  install_dir : join_paths(
    pkglibexecdir,
    multiarch,
    'i486',
  )
)
