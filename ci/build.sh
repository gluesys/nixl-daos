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
PKGS="gcc-c++ cmake pkg-config git python3-pip python3-devel ucx-devel libuuid-devel libaio-devel"
dnf -y -q install $PKGS >/dev/null 2>&1 || dnf -y install $PKGS | tail -1
# README: pip3 install meson ninja pybind11 tomlkit (python bindings need pybind11; tomlkit reads Cargo.lock)
pip3 -q install "meson>=1.3" ninja pybind11 tomlkit 2>/dev/null || pip3 install meson ninja pybind11 tomlkit | tail -1
cd /nixl
if ! meson setup build-daos --reconfigure -Ddaos_path=/usr -Ddisable_gds_backend=true -Ddisable_mooncake_backend=true -Ddisable_infinia_backend=true -Dbuildtype=release > build-daos-setup.log 2>&1; then
  echo "meson setup FAILED"; grep -E "ERROR|error:" build-daos-setup.log | tail -5; tail -20 build-daos-setup.log; exit 1
fi
grep -E "DAOS|daos_path|UCX|Message" build-daos-setup.log | head -20
ninja -C build-daos -j"$JOBS" 2>&1 | tail -15
echo "== plugin artifact"; find build-daos -name "libplugin_DAOS*.so" -exec ls -la {} \; ; ldd $(find build-daos -name "libplugin_DAOS*.so" | head -1) | grep -E "daos|not found" || true
echo "== install to DESTDIR install-daos (prefix /opt/nixl)"
meson configure build-daos -Dprefix=/opt/nixl >/dev/null
rm -rf install-daos; DESTDIR=/nixl/install-daos ninja -C build-daos install >/dev/null
ls install-daos/opt/nixl/lib64/plugins/
# The in-tree test only builds with -Dbuild_tests and a non-release buildtype; compile it
# directly against the installed tree so the release build stays as shipped.
P=install-daos/opt/nixl
g++ -std=c++20 -O2 -o $P/bin/nixl_daos_test test/unit/plugins/daos/nixl_daos_test.cpp \
    -I$P/include -I$P/include/nixl -Isrc/utils -Isrc/api/cpp -L$P/lib64 -lnixl -Wl,-rpath,/opt/nixl/lib64
ls install-daos/opt/nixl/bin/ | tr "\n" " "; echo'
echo "== runtime image"
$DOCKER build -q -f "$ROOT/images/Dockerfile.runtime" --build-arg BASE="$IMG" -t nixl-daos:dev "$NIXL/install-daos" && $DOCKER run --rm nixl-daos:dev bash -c 'ls $NIXL_PLUGIN_DIR; nixl_daos_test 2>&1 | head -2'
