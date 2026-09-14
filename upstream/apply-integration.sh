#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# Drop the DAOS plugin into a NIXL source tree and wire it into meson (root
# daos_dep + all_plugins entry + src/plugins gate + tests). Idempotent.
#
# The plugin's canonical source is exastor/lmcache-daos: nixl/plugin (backend)
# and nixl/tests. This repo's src/plugins/daos is an earlier skeleton kept only
# as a fallback when lmcache-daos is not checked out next to us.
#   usage: [PLUGIN_SRC=dir] [TESTS_SRC=dir] upstream/apply-integration.sh <nixl-src-dir>
set -euo pipefail
NIXL=${1:?nixl source dir}
HERE=$(cd "$(dirname "$0")/.." && pwd)
LMC="$HERE/../lmcache-daos/nixl"
PLUGIN_SRC=${PLUGIN_SRC:-$([ -d "$LMC/plugin" ] && echo "$LMC/plugin" || echo "$HERE/src/plugins/daos")}
TESTS_SRC=${TESTS_SRC:-$([ -d "$LMC/tests" ] && echo "$LMC/tests" || echo "")}
echo "plugin source: $PLUGIN_SRC"; [ -n "$TESTS_SRC" ] && echo "tests source:  $TESTS_SRC"

rm -rf "$NIXL/src/plugins/daos"; mkdir -p "$NIXL/src/plugins/daos"
cp "$PLUGIN_SRC"/*.cpp "$PLUGIN_SRC"/*.h "$PLUGIN_SRC"/meson.build "$NIXL/src/plugins/daos/"

# root meson.build: all_plugins entry + daos_dep (lmcache-daos/nixl/integration.patch)
grep -q "'DAOS'" "$NIXL/meson.build" || sed -i "s/all_plugins = \['UCX', 'LIBFABRIC', 'POSIX', /all_plugins = ['UCX', 'LIBFABRIC', 'POSIX', 'DAOS', /" "$NIXL/meson.build"
grep -q '^daos_dep = ' "$NIXL/meson.build" || python3 - "$NIXL/meson.build" <<'PY'
import sys; p=sys.argv[1]; s=open(p).read()
marker="# Check for libaio"
add="# DAOS client library (exastor/lmcache-daos nixl plugin). Looked up here so the\n# gate in src/plugins/meson.build can skip the subdir when it is absent.\ndaos_dep = meson.get_compiler('cpp').find_library('daos', required: false)\n\n"
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
# tests: build lmcache-daos/nixl/tests (test_reg, test_xfer, test_agent) as installable executables
if [ -n "$TESTS_SRC" ]; then
  rm -rf "$NIXL/test/unit/plugins/daos"; mkdir -p "$NIXL/test/unit/plugins/daos"
  cp "$TESTS_SRC"/test_reg.cpp "$TESTS_SRC"/test_xfer.cpp "$TESTS_SRC"/test_agent.cpp "$NIXL/test/unit/plugins/daos/"
  cat > "$NIXL/test/unit/plugins/daos/meson.build" <<'TST'
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
# Tests from exastor/lmcache-daos nixl/tests. Not registered with test(): they need a
# live DAOS pool/container. test_reg/test_xfer drive the backend class directly;
# test_agent goes through a real nixlAgent (needs NIXL_PLUGIN_DIR at run time).
daos_test_inc = [nixl_inc_dirs, utils_inc_dirs, include_directories('../../../../src/plugins/daos')]
foreach t : ['test_reg', 'test_xfer']
    executable('nixl_daos_' + t, t + '.cpp',
               dependencies: [nixl_infra, nixl_common_dep, daos_lib, daos_backend_interface, absl_log_dep],
               include_directories: daos_test_inc,
               # tests are written for a plain g++ build; NIXL adds -Werror
               cpp_args: daos_gpu_args + ['-Wno-error=shift-count-overflow'], install: true)
endforeach
executable('nixl_daos_test_agent', 'test_agent.cpp',
           dependencies: [nixl_dep, nixl_infra, absl_log_dep],
           include_directories: daos_test_inc, install: true)
TST
  python3 - "$NIXL/test/unit/plugins/meson.build" <<'PY'
import sys,re; p=sys.argv[1]; s=open(p).read()
s=re.sub(r"\n# DAOS backend test \(exastor/nixl-daos\).*?\nendif\n", "\n", s, flags=re.S)
if "subdir('daos')" not in s:
    s+="\n# DAOS backend tests (exastor/lmcache-daos)\nif enabled_plugins.get('DAOS') and not get_option('disable_daos_backend') and daos_dep.found()\n    subdir('daos')\nendif\n"
open(p,"w").write(s)
PY
fi
echo "integration applied to $NIXL"
