<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

<!-- Design-discussion issue for ai-dynamo/nixl (CONTRIBUTING: a large enhancement
     needs an issue before the PR). Body written in the What/Why/How form of
     upstream issue 1569.

     Posted 2026-10-08 and withdrawn 2026-10-09 with no replies, to finish tidying
     the documentation this draft points at. It goes up again as a NEW issue, not
     by reopening the old one, so the timeline starts clean. Update every
     "not posted yet" marker in upstream/README.md when it does. -->

**Title:** RFC: DAOS storage backend plugin

### **What?**

A NIXL storage backend for [DAOS](https://github.com/daos-stack/daos), the open-source distributed asynchronous object store (DAOS Foundation, Linux Foundation). The plugin moves data between application memory and DAOS objects through the DAOS object API. We have it working out of tree and would like to agree on the design before opening the PR, as CONTRIBUTING asks.

### **Why?**

NIXL has no DAOS backend today. A native backend lets Dynamo/KVBM and the LMCache NIXL backend use DAOS directly for KV cache offload.

**Performance:**

- **34.17 GB/s** reading 4.69 GiB (120 objects x 40 layers x 1 MiB) from 2 DAOS ranks (2.9.100 development build) over 400G verbs (`ofi+verbs;ofi_rxm`)
- Object API instead of the DFS file layer: on the same 4800 reads of 1 MiB, DFS costs 0.63 ms fixed per object and the object API 0.0137 ms. At 1 MiB, DFS spends about 90% of its time on overhead
- A whole descriptor list becomes one `daos_obj_fetch()`/`daos_obj_update()` through the DAOS iod array

**Reliability:**

- Every request waits on a DAOS event queue with a deadline, so a dead engine cannot hang a caller. With `daos_server` killed mid-read, the blocking path was still stuck at 120 s; an 8 s deadline returned in 8 s with every thread reclaimed. The deadline costs nothing measurable (30.64 vs 31.04 GB/s, ranges overlapping)
- Object ids are derived from the caller's key, so the same key names the same object across process restarts

**Flexibility:**

- `DRAM_SEG` (local) and `FILE_SEG` (DAOS objects); `VRAM_SEG` when the DAOS client exports GPU-direct entry points
- Pool and container chosen per descriptor (`metaInfo`), so one engine serves several containers

Out-of-tree plugin: https://github.com/gluesys/nixl-daos ([`src/plugins/daos/README.md`](https://github.com/gluesys/nixl-daos/blob/main/src/plugins/daos/README.md) summarizes the measurements and links each method).

### **How?**

- `src/plugins/daos/`: `nixlDaosEngine` implementing `nixlBackendEngine`; local transfers only (`supportsLocal()`)
- Descriptor mapping: `metaInfo` = `"pool/container"` (or with an explicit object id), `devId` = object key, `addr` = offset. Offsets map to `dkey = addr / 64 MiB`, `akey = addr % 64 MiB`, so descriptors within one span fold into a single RPC and separate spans spread across targets
- Fixed worker pool and an event-queue deadline, both backend parameters (`threads`, `eq_timeout`, advertised by `getPluginParams()`) with an environment form for a whole process; defaults 64 threads and 60 s
- `queryMem()` answers whether an object is present and how far it extends, so a cache can skip a read. Objects use `DAOS_OT_MULTI_UINT64` because the dkey/akey are uint64 and an ordered query needs that
- Tests are built but not registered with `meson test`, since they need a live pool -- as in `infinia`, `hf3fs`, `mooncake` and `object`
- Build: `-Ddaos_path` points at the DAOS client; skipped with a warning when the client is absent, an error when requested explicitly, and `-Ddisable_plugins=DAOS` leaves it out
- Tests in `test/unit/plugins/daos/` (`test_reg`, `test_xfer`, `test_agent`); they need a live pool, so they are built but not registered with `meson test`

Copyright headers follow the INFINIA plugin: an NVIDIA `SPDX-FileCopyrightText` line next to the Gluesys one, which `.github/workflows/copyright-check.sh` accepts as it stands. Say the word if you would rather add "Gluesys Co., Ltd" to `AUTHORS` instead, as was done for AMD.

**Status:** on NIXL main (44c1b56) it builds with `-Werror` against DAOS 2.8 (`daos-devel` 2.8.0-6), is clang-format clean, and passes all three tests against a DAOS 2.8.0-6 server (1 rank, `ofi+tcp`, client in a Kubernetes pod with `daos_agent` as a sidecar).

### **Questions for the NIXL team**

1. **CI coverage.** The NIXL CI images are Ubuntu-based; DAOS publishes packages for EL8/EL9 and SUSE, not Ubuntu. Options we see: (a) compile-only in CI from a small DAOS client build in a side image, the way `Dockerfile.infinia-libs` supplies Infinia libraries; (b) build the plugin in an EL9-based job with `daos-devel` from packages.daos.io; (c) no DAOS in NIXL CI at first, with results from our CI attached to PRs. Which would you prefer? We have built and checked (a): [`Dockerfile.daos-libs`](https://github.com/gluesys/nixl-daos/blob/main/upstream/ci/Dockerfile.daos-libs) builds the DAOS v2.8.0 client from source with DAOS's own Ubuntu procedure and keeps only headers and libraries (23 MB), and on Ubuntu 24.04 NIXL main then builds the plugin and its tests with `--buildtype=debug` (312/312 targets). Without DAOS the plugin is skipped with a warning and the rest of NIXL builds. We can also provide (b).
2. **GPU-direct.** `VRAM_SEG` depends on a DAOS client with GPU support that is not in a released DAOS yet, and is currently about 3.4x slower than staging through host memory. Should we drop it from the first PR and add it later?
### **Maintenance**

We run this backend in our own product (a Kubernetes operator for DAOS and an
LMCache KV-cache path), so it is exercised outside NIXL's CI as well. We will
keep it building against NIXL main, follow DAOS releases, and fix it when an
upstream refactor moves the interface under it. We would add ourselves to
`CODEOWNERS` for `src/plugins/daos` the way the libfabric plugin does, so review
requests reach someone.

### **Prior contribution**

While writing this backend we found the `customParams` null dereference in `nixlBackendEngine`: reported in #2245 and fixed by #2246 (merged).
