#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Gluesys Co., Ltd.
#
# Run nixl_daos_test against a live DAOS system from a client host that has
# daos_agent configured (e.g. daos_ci 192.168.34.20 -> servers 34.21/22 over ib0).
# Non-destructive: starts daos_agent if inactive and creates one container in an
# existing pool. Never formats or restarts servers.
#   usage: DOCKER="sudo docker" ci/e2e-testbed.sh root@192.168.34.20 nvme_pool daos_flexa [image] [cont]
set -euo pipefail
HOST=${1:?client host (user@ip)}; POOL=${2:?pool}; SYS=${3:?daos system name}
IMG=${4:-nixl-daos:dev}; CONT=${5:-nixltest}
DOCKER=${DOCKER:-docker}; SSH=${SSH:-ssh -o StrictHostKeyChecking=no}; SCP=${SCP:-scp -o StrictHostKeyChecking=no}
TAR=/tmp/nixl-daos-$(date +%s).tar
if [ "${SKIP_SHIP:-0}" != 1 ]; then
  echo "== ship image $IMG to $HOST"
  $DOCKER save "$IMG" > "$TAR"; $SCP -q "$TAR" "$HOST:/tmp/nixl-daos.tar"; rm -f "$TAR"
fi
$SSH "$HOST" bash -s "$POOL" "$SYS" "$IMG" "$CONT" <<'REMOTE'
set -euo pipefail
POOL=$1; SYS=$2; IMG=$3; CONT=$4
[ -f /tmp/nixl-daos.tar ] && { podman load -q -i /tmp/nixl-daos.tar >/dev/null && rm -f /tmp/nixl-daos.tar; } || true
systemctl is-active daos_agent >/dev/null || { systemctl start daos_agent || true; sleep 12; }
systemctl is-active daos_agent
RUN="podman run --rm --network host --ulimit memlock=-1:-1 --device /dev/infiniband -v /var/run/daos_agent:/var/run/daos_agent -v /etc/daos:/etc/daos:ro -e DAOS_AGENT_DRPC_DIR=/var/run/daos_agent $IMG"
echo "== pool"; $RUN daos pool query "$POOL" | head -6
echo "== container (create if missing)"; $RUN daos cont query "$POOL" "$CONT" >/dev/null 2>&1 || $RUN daos cont create "$POOL" "$CONT" --type POSIX --properties rd_fac:0 | tail -2
echo "== plugin load check"; $RUN bash -c 'ls $NIXL_PLUGIN_DIR; ldd $NIXL_PLUGIN_DIR/libplugin_DAOS.so | grep -E "daos|not found"'
echo "== nixl_daos_test (4 objects x 1 MiB)"; $RUN nixl_daos_test --pool "$POOL" --container "$CONT" --sys "$SYS" -n 4 -s 1048576
echo "== nixl_daos_test layerwise (8 objects x 4 layers x 256 KiB)"; $RUN nixl_daos_test --pool "$POOL" --container "$CONT" --sys "$SYS" -n 8 -s 262144 -l 4
REMOTE
