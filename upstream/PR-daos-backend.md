<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

<!-- Draft PR body for ai-dynamo/nixl, in its template (What / Why / How).
     Branch: feat/daos-backend on top of ai-dynamo/nixl main. Open only after the
     design issue (ISSUE-daos-backend.md) is agreed. Not posted yet. -->

**Title:** plugins: add DAOS storage backend

## What?

A new storage backend plugin, `DAOS`, for the DAOS distributed object store.

- `src/plugins/daos/`: `nixlDaosEngine` (`daos_backend.{h,cpp}`), plugin entry
  (`daos_plugin.cpp`), `meson.build`, `README.md`.
- `test/unit/plugins/daos/`: `nixl_daos_test_reg`, `nixl_daos_test_xfer`,
  `nixl_daos_test_agent` (need a live DAOS pool; built and installed, not
  registered with `meson test`).
- Build: `DAOS` in `all_plugins`; `-Ddisable_daos_backend`, `-Ddaos_path`.

## Why?

NIXL has no DAOS backend. DAOS is an open-source distributed object store used
for HPC and AI storage; a NIXL backend lets Dynamo/KVBM and the LMCache NIXL
backend use it directly. Design discussed in #<issue>.

## How?

- **Object API, not DFS.** One NIXL descriptor list becomes one
  `daos_obj_fetch()`/`daos_obj_update()` through the iod array. DFS costs 0.63 ms
  fixed per object against 0.0137 ms for the object API.
- **Mapping.** `metaInfo` = `"pool/container"` (optionally with an object id),
  `devId` = object key (object id derived from it, stable across restarts),
  `addr` = offset; `dkey = addr / 64 MiB`, `akey = addr % 64 MiB`.
- **Concurrency.** A fixed worker pool (`NIXL_DAOS_THREADS`, default 64). Each
  request borrows a DAOS event queue for its duration and polls it with a deadline
  (`NIXL_DAOS_EQ_TIMEOUT`, default 60 s), so a dead engine cannot hang a request.
- **Build.** Follows INFINIA: skipped with a warning when the DAOS client is not
  found, an error when requested explicitly.

### Testing

- Build: `meson setup build -Denable_plugins=DAOS,POSIX && ninja -C build` against
  DAOS 2.8 (`daos-devel`), clang-format-19 clean.
- Runtime (DAOS 2.8, 2 ranks, 400G verbs): `test_reg`, `test_xfer`, `test_agent`
  pass; 34.17 GB/s reading 4.69 GiB.

## Checklist

- [ ] `.clang-format` applied
- [ ] Follows `docs/CodeStyle.md`
- [ ] Tests added
- [ ] Documentation (`src/plugins/daos/README.md`)
- [ ] Commits signed off (DCO)
