#!/bin/sh
# Copyright © 2016-2018 Simon McVittie
# Copyright © 2018-2023 Collabora Ltd.
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

set -eu

if [ -z "${G_TEST_SRCDIR-}" ]; then
    me="$(readlink -f "$0")"
    G_TEST_SRCDIR="${me%/*}"
fi

cd "$G_TEST_SRCDIR/.."
echo "TAP version 13"

if [ "${PYCODESTYLE:=pycodestyle}" = false ] || \
        [ -z "$(command -v "$PYCODESTYLE")" ]; then
    echo "1..0 # SKIP pycodestyle not found"
    exit 0
fi

i=0
for script in \
    ./bin/*.py \
    ./build-aux/*.py \
    ./pressure-vessel/*.py \
    ./subprojects/container-runtime/*.py \
    ./subprojects/container-runtime/tests/depot/*.py \
    ./tests/*.py \
    ./tests/*/*.py \
; do
    if ! [ -e "$script" ]; then
        continue
    fi

    # Ignore E402: when using GObject-Introspection, not all imports
    # can come first
    # Ignore W503: allow wrapping long expressions before a binary operator
    i=$((i + 1))
    if [ "${PYCODESTYLE}" = false ] || \
            [ -z "$(command -v "$PYCODESTYLE")" ]; then
        echo "ok $i - $script # SKIP pycodestyle not found"
    elif "${PYCODESTYLE}" \
            --ignore=E402,W503 \
            "$script" >&2; then
        echo "ok $i - $script"
    elif [ -n "${LINT_WARNINGS_ARE_ERRORS-}" ]; then
        echo "not ok $i - $script"
    else
        echo "not ok $i - $script # TO""DO pycodestyle issues reported"
    fi
done

if [ "$i" = 0 ]; then
    echo "1..0 # SKIP no Python scripts to test"
else
    echo "1..$i"
fi

# vim:set sw=4 sts=4 et:
