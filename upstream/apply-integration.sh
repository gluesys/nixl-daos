#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# Drop src/plugins/daos into a NIXL source tree and wire it into meson, the same
# way the infinia plugin is wired (path option + disable switch). Idempotent.
#   usage: upstream/apply-integration.sh <nixl-src-dir>
set -euo pipefail
NIXL=${1:?nixl source dir}
HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$NIXL/src/plugins/daos"
cp "$HERE"/src/plugins/daos/*.cpp "$HERE"/src/plugins/daos/*.h "$HERE"/src/plugins/daos/meson.build "$NIXL/src/plugins/daos/"
grep -q "option('daos_path'" "$NIXL/meson_options.txt" || cat >> "$NIXL/meson_options.txt" <<'OPT'
option('daos_path', type: 'string', value: '/usr', description: 'Path to DAOS client install (lib64/libdaos.so, include/daos.h)')
option('disable_daos_backend', type : 'boolean', value : false, description : 'disable DAOS backend')
option('daos_gpu', type : 'boolean', value : false, description : 'DAOS backend: enable VRAM_SEG (needs a GPU-direct capable libdaos)')
OPT
if ! grep -q "subdir('daos')" "$NIXL/src/plugins/meson.build"; then
  # insert right before the telemetry subdir so plugin ordering matches upstream style
  python3 - "$NIXL/src/plugins/meson.build" <<'PY'
import sys; p=sys.argv[1]; s=open(p).read()
block='''
# DAOS backend (exastor/nixl-daos). Wired like infinia: path option + disable switch.
daos_path = get_option('daos_path')
daos_lib_found = cc.find_library('daos', dirs: [daos_path + '/lib64', daos_path + '/lib'], required: false)
disable_daos_backend = get_option('disable_daos_backend')
if enabled_plugins.get('DAOS', true)
    if (disable_daos_backend or not daos_lib_found.found()) and is_explicit_enable
        if disable_daos_backend
            error('DAOS plugin requested but DAOS backend is disabled')
        else
            error('DAOS plugin requested but libdaos not found under ' + daos_path)
        endif
    elif not disable_daos_backend and daos_lib_found.found()
        subdir('daos')
    endif
endif

'''
marker="subdir('telemetry')"
assert marker in s, "telemetry marker not found"
s=s.replace(marker, block+marker,1); open(p,"w").write(s)
PY
fi
echo "integration applied to $NIXL"
