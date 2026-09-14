#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# Build NIXL + the DAOS plugin inside the daos-client image (has libdaos, daos-devel).
#   usage: ci/build.sh [nixl-src-dir] [daos-client-image] [jobs]
# Produces $NIXL/build-daos with libplugin_DAOS.so. Network is needed once for
# meson wraps (abseil-cpp, asio, taskflow, tomlplusplus, liburing).
set -euo pipefail
NIXL=${1:-$(cd "$(dirname "$0")/../../nixl" && pwd)}
IMG=${2:-daos/daos-client:dev}
JOBS=${3:-2}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DOCKER=${DOCKER:-docker}
"$ROOT/upstream/apply-integration.sh" "$NIXL"
$DOCKER run --rm -v "$NIXL:/nixl" -v "$ROOT:/p:ro" -e JOBS="$JOBS" "$IMG" bash -c '
set -euo pipefail
dnf -y -q install gcc-c++ cmake pkg-config git python3-pip ucx-devel libuuid-devel libaio-devel >/dev/null 2>&1 || dnf -y install gcc-c++ cmake pkg-config git python3-pip ucx-devel libuuid-devel libaio-devel | tail -1
pip3 -q install "meson>=1.3" ninja 2>/dev/null || pip3 install meson ninja | tail -1
cd /nixl
meson setup build-daos --reconfigure -Ddaos_path=/usr -Ddisable_gds_backend=true -Ddisable_mooncake_backend=true -Ddisable_infinia_backend=true -Dbuildtype=release 2>&1 | grep -E "DAOS|daos|UCX|Message|WARNING|ERROR|Found|abseil" | head -40 || true
ninja -C build-daos -j"$JOBS" 2>&1 | tail -15
echo "== plugin artifact"; find build-daos -name "libplugin_DAOS*.so" -exec ls -la {} \; ; ldd $(find build-daos -name "libplugin_DAOS*.so" | head -1) | grep -E "daos|not found" || true'
