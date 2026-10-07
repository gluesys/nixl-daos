<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# ADR-nixl-001: OBJ_SEG + libdaos raw object API, no DFS

- Status: **Superseded.** The design of the plugin that lived under
  `nixl/plugin` in lmcache-daos (from 2026-09-12; now `src/plugins/daos/` here)
  is canonical: FILE_SEG, `metaInfo = "pool/container"`, `devId` → object id,
  `dkey = addr / 64 MiB` spans. The conclusion "no DFS, raw object API" stands.
- Date: 2026-09-14

## Context
A NIXL backend receives a descriptor list (many small segments) as one request.
The layerwise measurements in lmcache-daos isolated a serial fixed cost of about
0.63 ms per object on the DFS path (9-10% worse going from 16 to 128 workers).
Rewriting the same 4800 x 1 MiB reads on the raw object API (dkey = chunk,
akey = layer) took 3343 ms → 245 ms, and 204 ms as one RPC with an iod array
([measurement](../measurements/LAYERWISE-MEASUREMENT.md)).

## Decision
Register as OBJ_SEG (key + bucket), not FILE_SEG (fd + offset, DFS), and call
`daos_obj_update`/`daos_obj_fetch` with an iod array. bucket = container UUID
(the pool is a backend parameter), key = chunk hash, akey = layer index from the
descriptor `metaInfo`.

## Consequences and trade-offs
- Not compatible with the lmcache-daos on-disk formats v1/v2 (DFS files). The
  NIXL path uses its own container/namespace.
- The container need not be of POSIX type, so it is separate from what the CSI
  driver mounts.

## When to revisit
Re-measure if DAOS 3.0 structurally reduces the DFS fixed cost (e.g. the HPE DFS
client-side cache).
