# Copyright © 2019-2020 Collabora Ltd.
#
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

test_env = environment(common_test_env_vars + uninstalled_env_vars)
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
test_env.set(
  'PRESSURE_VESSEL_TEST_CONTAINERS',
  get_option('test_containers_dir'),
)
test_env.prepend('PYTHONPATH', meson.current_source_dir() / '..')

# Helper programs and manual tests
helpers = [
  'cheap-copy',
  'helper',
  'mtree-apply',
]

foreach helper : helpers
  test_depends += executable(
    'test-' + helper,
    sources : [
      helper + '.c',
    ],
    dependencies : [
      gio_unix,
      json_glib,
      libglnx_dep,
      test_utils_static_libsteamrt_dep,
    ],
    link_with : [
      pressure_vessel_utils,
    ],
    include_directories : pv_include_dirs,
    install : false,
  )
endforeach

tests = [
  'adverb.py',
  'cheap-copy.py',
  'containers.py',
  'invocation.py',
  'launcher.py',
  'mtree-apply.py',
  'test-locale-gen.sh',
  'utils.py',
]

compiled_tests = [
  {'name': 'adverb-preload', 'adverb': true},
  {'name': 'bwrap-lock'},
  {'name': 'graphics-provider', 'wrap': true},
  {'name': 'resolve-in-sysroot'},
  {'name': 'wait-for-child-processes'},
  {'name': 'wrap-setup', 'wrap': true},
  {'name': 'utils'},
]

foreach test_info : compiled_tests
  test_name = test_info['name']

  # For whatever reason, linking to both -adverb and -wrap code in a Scout
  # environment results in the json-glib dependency not being found.
  # None of our tests actually need both, so only link to one (or neither).
  if test_info.get('adverb', false)
    pv_dep = pressure_vessel_adverb_lib_dep
  elif test_info.get('wrap', false)
    pv_dep = pressure_vessel_wrap_lib_dep
  else
    pv_dep = pressure_vessel_utils_dep
  endif

  exe = executable(
    'test-' + test_name,
    files(test_name + '.c'),
    dependencies : [
      gio_unix,
      libglnx_dep,
      # Keep libsteamrt_dep at the bottom to avoid linking errors with Scout
      pv_dep,
      test_utils_static_libsteamrt_dep,
    ],
    include_directories : pv_include_dirs,
    install : false,
  )

  if glib_tap_support
    test(
      test_name, exe,
      args : ['--tap'],
      depends : test_depends,
      env : test_env,
      protocol : 'tap',
      suite : ['pressure-vessel'],
    )
  else
    test(
      test_name, exe,
      depends : test_depends,
      env : test_env,
      suite : ['pressure-vessel'],
    )
  endif

endforeach

foreach test_name : tests
  test_argv = []
  timeout = 30

  if test_name.endswith('.py')
    test_argv += [python.full_path(), '-S']
  endif

  test_argv += [files(test_name)]

  if test_name in ['test-locale-gen.sh', 'launcher.py']
    timeout = 90
  endif

  if test_name in ['containers.py']
    timeout = 300
  endif

  if dbus_run_session.found()
    test(
      test_name, dbus_run_session,
      args : [
        '--config-file', no_activation_session_conf,
        '--',
      ] + test_argv,
      depends : test_depends,
      env : test_env,
      protocol : 'tap',
      suite : ['pressure-vessel'],
      timeout : timeout,
    )
    # Re-run launcher.py with the default python3 version (if any)
    # to try the code path involving gi.repository.Gio
    if test_name == 'launcher.py' and full_python.found()
      test(
        'full-python-' + test_name,
        dbus_run_session,
        args : [
          '--',
          full_python.full_path(),
          files(test_name),
        ],
        depends : test_depends,
        env : test_env,
        protocol : 'tap',
        suite : ['pressure-vessel'],
        timeout : timeout,
      )
    endif
  endif
endforeach

# vim:set sw=2 sts=2 et:
