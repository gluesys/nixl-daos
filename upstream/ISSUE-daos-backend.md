<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

<!-- Design-discussion issue for ai-dynamo/nixl (CONTRIBUTING: a large enhancement
     needs an issue before the PR). Posted 2026-10-08 as
     https://github.com/ai-dynamo/nixl/issues/2361 -->

**Title:** RFC: DAOS storage backend plugin

## Summary

We would like to contribute a NIXL backend for [DAOS](https://github.com/daos-stack/daos),
the open-source distributed asynchronous object store (DAOS Foundation, Linux
Foundation). NIXL has no DAOS backend today. We have one working out of tree and
would like to agree on the design before opening the PR, as CONTRIBUTING asks.

## What it is

- `src/plugins/daos/`: `nixlDaosEngine` on the DAOS **object API** (dkey/akey),
  not the DFS file layer.
- Segments: `DRAM_SEG` local, `FILE_SEG` for DAOS objects. `VRAM_SEG` only when the
  DAOS client exports GPU-direct entry points (checked at configure time).
- Local transfers only (`supportsLocal()`).
- Descriptor mapping: `metaInfo` = `"pool/container"` (or with an explicit object
  id), `devId` = the object key (the object id is derived from it, so it is stable
  across restarts), `addr` = offset in the object. Offsets map to
  `dkey = addr / 64 MiB`, `akey = addr % 64 MiB`, so descriptors in one span fold
  into a single `daos_obj_fetch()`/`daos_obj_update()`.
- Build: follows the INFINIA pattern -- `-Ddisable_daos_backend`, `-Ddaos_path`,
  skipped with a warning when the DAOS client is absent, an error when requested
  explicitly.

## Why the object API

Measured on the same 4800 reads of 1 MiB: DFS costs 0.63 ms fixed per object, the
object API 0.0137 ms. At 1 MiB DFS spends ~90% of the time on overhead, and the
object API maps a NIXL descriptor list onto one RPC through its iod array.

Results (2 DAOS 2.8 ranks, 400G verbs, `ofi+verbs;ofi_rxm`): **34.17 GB/s** reading
4.69 GiB through the plugin. Measurements and methods:
https://github.com/gluesys/lmcache-daos/tree/main/doc (NIXL-DAOS-MEASUREMENT.md,
LAYERWISE-MEASUREMENT.md, FAILURE-MODES.md).

## Questions for the NIXL team

1. **CI coverage.** The NIXL CI images are Ubuntu-based; DAOS publishes packages
   for EL8/EL9 and SUSE, not Ubuntu. Options we see:
   (a) compile-only in CI from a small DAOS client build in a side image, the way
       `Dockerfile.infinia-libs` supplies Infinia libraries;
   (b) build the plugin in an EL9-based job with `daos-devel` from packages.daos.io;
   (c) no DAOS in NIXL CI at first, with results from our CI attached to PRs.
   Which would you prefer? We can provide (a) or (b).
2. **Runtime tests.** Our tests need a live DAOS pool, so they are built but not
   registered with `meson test` (like `test/unit/plugins/infinia`). Is that
   acceptable for a first version, or do you want GoogleTest cases that skip when
   no pool is configured?
3. **Configuration.** Thread-pool size and the event-queue deadline are read from
   environment variables (`NIXL_DAOS_THREADS`, `NIXL_DAOS_EQ_TIMEOUT`). We can move
   them to backend parameters (`getPluginParams()`) if that is the convention you
   want for new plugins.
4. **GPU-direct.** `VRAM_SEG` depends on a DAOS client with GPU support that is not
   in a released DAOS yet, and is currently slower than staging through host
   memory. Should we drop it from the first PR and add it later?
5. **Copyright header.** `.github/workflows/copyright-check.sh` accepts a file only
   with an `SPDX-FileCopyrightText` line from the `AUTHORS` list (NVIDIA, AMD). The new
   files are written by Gluesys. Should we add the NVIDIA line next to ours, as the
   INFINIA plugin did, or would you add "Gluesys Co., Ltd" to `AUTHORS`, as was done
   for AMD?

## Prior contribution

We found and fixed the `customParams` null dereference in `nixlBackendEngine`
while writing this backend (#2245, #2246, merged).
