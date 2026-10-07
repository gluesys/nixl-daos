#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# Drop the DAOS plugin into a NIXL source tree and wire it into meson (root
# daos_dep + all_plugins entry + src/plugins gate + tests). Idempotent.
#
# The plugin lives in this repo: src/plugins/daos (backend + README) and
# test/unit/plugins/daos (tests). It moved here from exastor/lmcache-daos nixl/
# on 2026-10-08.
#   usage: [PLUGIN_SRC=dir] [TESTS_SRC=dir] upstream/apply-integration.sh <nixl-src-dir>
set -euo pipefail
NIXL=${1:?nixl source dir}
HERE=$(cd "$(dirname "$0")/.." && pwd)
PLUGIN_SRC=${PLUGIN_SRC:-$HERE/src/plugins/daos}
TESTS_SRC=${TESTS_SRC:-$HERE/test/unit/plugins/daos}
echo "plugin source: $PLUGIN_SRC"; [ -n "$TESTS_SRC" ] && echo "tests source:  $TESTS_SRC"

rm -rf "$NIXL/src/plugins/daos"; mkdir -p "$NIXL/src/plugins/daos"
cp "$PLUGIN_SRC"/*.cpp "$PLUGIN_SRC"/*.h "$PLUGIN_SRC"/meson.build "$PLUGIN_SRC"/README.md "$NIXL/src/plugins/daos/"

# root meson.build: all_plugins entry + daos_dep (upstream/integration.patch)
grep -q "'DAOS'" "$NIXL/meson.build" || sed -i "s/all_plugins = \['UCX', 'LIBFABRIC', 'POSIX', /all_plugins = ['UCX', 'LIBFABRIC', 'POSIX', 'DAOS', /" "$NIXL/meson.build"
grep -q '^daos_dep = ' "$NIXL/meson.build" || python3 - "$NIXL/meson.build" <<'PY'
import sys; p=sys.argv[1]; s=open(p).read()
marker="# Check for libaio"
add="# DAOS client library (exastor/nixl-daos plugin). Looked up here so the\n# gate in src/plugins/meson.build can skip the subdir when it is absent.\ndaos_dep = meson.get_compiler('cpp').find_library('daos', required: false)\n\n"
assert marker in s; s=s.replace(marker, add+marker, 1); open(p,"w").write(s)
PY
# options kept for CI convenience (plugin meson does its own prefix search via DAOS_PREFIX)
grep -q "option('disable_daos_backend'" "$NIXL/meson_options.txt" || cat >> "$NIXL/meson_options.txt" <<'OPT'
option('disable_daos_backend', type : 'boolean', value : false, description : 'disable DAOS backend')
OPT
# src/plugins gate
python3 - "$NIXL/src/plugins/meson.build" <<'PY'
import sys,re; p=sys.argv[1]; s=open(p).read()
# drop an older block from this script, if present
s=re.sub(r"\n# DAOS backend \(exastor/nixl-daos\).*?\nendif\n\n", "\n", s, flags=re.S)
if "subdir('daos')" not in s:
    block="""if enabled_plugins.get('DAOS') and not get_option('disable_daos_backend')
    if not daos_dep.found() and is_explicit_enable
        error('DAOS plugin requested but libdaos not found (install daos-devel or set DAOS_PREFIX)')
    elif daos_dep.found()
        subdir('daos')
    endif
endif

"""
    marker="if enabled_plugins.get('OBJ')"; assert marker in s
    s=s.replace(marker, block+marker, 1)
open(p,"w").write(s)
PY
# tests: build test/unit/plugins/daos (test_reg, test_xfer, test_agent) as installable executables
if [ -n "$TESTS_SRC" ]; then
  rm -rf "$NIXL/test/unit/plugins/daos"; mkdir -p "$NIXL/test/unit/plugins/daos"
  cp "$TESTS_SRC"/test_reg.cpp "$TESTS_SRC"/test_xfer.cpp "$TESTS_SRC"/test_agent.cpp "$NIXL/test/unit/plugins/daos/"
  cp "$TESTS_SRC"/meson.build "$NIXL/test/unit/plugins/daos/"
  python3 - "$NIXL/test/unit/plugins/meson.build" <<'PY'
import sys,re; p=sys.argv[1]; s=open(p).read()
s=re.sub(r"\n# DAOS backend test \(exastor/nixl-daos\).*?\nendif\n", "\n", s, flags=re.S)
if "subdir('daos')" not in s:
    s+="\n# DAOS backend tests (exastor/nixl-daos)\nif enabled_plugins.get('DAOS') and not get_option('disable_daos_backend') and daos_dep.found()\n    subdir('daos')\nendif\n"
open(p,"w").write(s)
PY
fi
echo "integration applied to $NIXL"
