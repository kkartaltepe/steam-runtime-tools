# Copyright 2021 Collabora Ltd.
# SPDX-License-Identifier: MIT

srt_helpers += shared_module(
  'identify-lib',
  '../identify-lib.c',
  c_args : ['-D_SRT_LIB_VALUE="lib64"',],
  install : true,
  install_dir : join_paths(
    pkglibexecdir,
    multiarch,
    'lib64',
  )
)
