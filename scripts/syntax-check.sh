#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# Compile-check src/plugins/daos against upstream NIXL headers and the DAOS 2.8
# client headers, inside the daos-client image. No meson, no linking: this is
# the cheapest gate that catches API drift on either side.
#   usage: scripts/syntax-check.sh [nixl-src-dir] [daos-client-image]
set -euo pipefail
NIXL=${1:-$(cd "$(dirname "$0")/../../nixl" && pwd)}
IMG=${2:-daos/daos-client:dev}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DOCKER=${DOCKER:-docker}
$DOCKER run --rm -v "$NIXL:/nixl:ro" -v "$ROOT:/p:ro" "$IMG" bash -c '
set -e
dnf -y -q install gcc-c++ git libuuid-devel >/dev/null 2>&1 || true
# NIXL headers need a newer abseil than EPEL 9 ships (absl/log); headers only.
git clone -q --depth 1 -b 20250127.1 https://github.com/abseil/abseil-cpp /absl 2>/dev/null || true
INC="$(find /nixl/src -type d | sed "s/^/-I/" | tr "\n" " ") -I/absl -I/usr/include"
rc=0
for f in daos_plugin.cpp daos_backend.cpp; do
  echo "== $f"
  g++ -std=c++20 -fsyntax-only -Wall $INC /p/src/plugins/daos/$f || rc=1
done
exit $rc'
