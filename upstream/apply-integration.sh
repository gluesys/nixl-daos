#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# Drop the DAOS plugin into a NIXL source tree and wire it into meson. Idempotent.
#
# The plugin lives in this repo: src/plugins/daos (backend + README) and
# test/unit/plugins/daos (tests), in the form proposed upstream. The meson wiring
# (DAOS in all_plugins, -Ddisable_daos_backend, -Ddaos_path, the src/plugins and
# test/unit/plugins gates) is upstream/integration.patch. The whole change as one
# upstream commit is upstream/0001-plugins-add-DAOS-storage-backend.patch.
#   usage: [PLUGIN_SRC=dir] [TESTS_SRC=dir] upstream/apply-integration.sh <nixl-src-dir>
set -euo pipefail
NIXL=${1:?nixl source dir}
HERE=$(cd "$(dirname "$0")/.." && pwd)
PLUGIN_SRC=${PLUGIN_SRC:-$HERE/src/plugins/daos}
TESTS_SRC=${TESTS_SRC:-$HERE/test/unit/plugins/daos}
echo "plugin source: $PLUGIN_SRC"; echo "tests source:  $TESTS_SRC"

mkdir -p "$NIXL/src/plugins/daos" "$NIXL/test/unit/plugins/daos"
cp "$PLUGIN_SRC"/*.cpp "$PLUGIN_SRC"/*.h "$PLUGIN_SRC"/meson.build "$PLUGIN_SRC"/README.md "$NIXL/src/plugins/daos/"
cp "$TESTS_SRC"/test_reg.cpp "$TESTS_SRC"/test_xfer.cpp "$TESTS_SRC"/test_agent.cpp "$TESTS_SRC"/meson.build "$NIXL/test/unit/plugins/daos/"

# meson wiring: skip when the tree already has it (DAOS in all_plugins)
if grep -q "'DAOS'" "$NIXL/meson.build"; then
    echo "meson wiring already present"
elif (cd "$NIXL" && patch -p1 --dry-run -s < "$HERE/upstream/integration.patch" >/dev/null); then
    (cd "$NIXL" && patch -p1 -s < "$HERE/upstream/integration.patch")
    echo "meson wiring applied"
else
    echo "upstream/integration.patch does not apply to $NIXL (NIXL main moved?); rebase it" >&2
    exit 1
fi
echo "integration applied to $NIXL"
