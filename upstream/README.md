<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# Upstream (ai-dynamo/nixl) submission

| file | contents |
|---|---|
| `0001-plugins-add-DAOS-storage-backend.patch` | one commit on upstream main (DCO signed off): the whole change the PR will carry |
| `integration.patch` | the meson wiring part of that commit (`meson.build`, `meson_options.txt`, `src/plugins/meson.build`, `test/unit/plugins/meson.build`) |
| `apply-integration.sh` | puts this repository's plugin and tests into a NIXL tree and applies `integration.patch`; used by CI (`ci/build.sh`) |
| `ISSUE-daos-backend.md` | the upstream design-discussion issue. **Posted: [ai-dynamo/nixl#2361](https://github.com/ai-dynamo/nixl/issues/2361) (2026-10-08)** |
| `PR-daos-backend.md` | draft PR body (upstream template What/Why/How; not posted) |
| `ci/Dockerfile.daos-libs` | DAOS client headers and libraries for Ubuntu as a small data image, the way upstream's `Dockerfile.infinia-libs` supplies Infinia (answer (a) to question 1 of the issue) |
| `ci/Dockerfile.ubuntu-check` | builds NIXL + the DAOS plugin and tests on Ubuntu 24.04 from that image, with upstream CI's build settings |

## Base and verification (2026-10-08)

- Base: ai-dynamo/nixl main `44c1b56`. Local branch `feat/daos-backend` (worktree
  `../nixl-wt-daos`, commit `d2fb78d`).
- Form: follows upstream CONTRIBUTING and the most recent external plugin
  (INFINIA, DDN): `-Ddisable_daos_backend` and `-Ddaos_path`; skipped with a
  warning when the DAOS client is absent (an error when requested explicitly);
  SPDX headers; a plugin `README.md` (Overview, Dependencies, Build, API,
  Example); clang-format 19.
- Build: in the daos-client 2.8 image with
  `-Denable_plugins=DAOS,POSIX -Dbuild_tests=true` and `-Werror`. It produces
  `libplugin_DAOS.so` and the three tests. `clang-format-19 --dry-run --Werror`
  passes.
- Run (2026-10-08, CI lane B): an image built from this branch (base
  `daos-client:2.8.0-20261006-g41c3647d`, libfabric 1.22) against a DAOS 2.8.0-6
  server, 1 rank (`ofi+tcp`, operator-managed): `test_xfer` 10/10, `test_reg`
  11/11, `test_agent` 11/11. The client is a pod with a `daos_agent` sidecar
  (the same shape as the operator's S3 gateway).
- Found and fixed in that run: the engine destructor called `daos_fini()` before
  the event-queue pool was released, so every shutdown logged `DER_BUSY`
  (`crt_finalize: cannot finalize, current ctx_num(1)`) and then called
  `daos_eq_destroy()` on a library already shut down. Releasing the queue pool
  first removed the error.
- Ubuntu 24.04 (the NIXL CI base), 2026-10-08:
  - Without DAOS: default setup warns and skips the plugin, and NIXL builds
    (283/283); `-Denable_plugins=DAOS` fails with a clear error;
    `-Ddisable_daos_backend=true` leaves it out silently.
  - With DAOS v2.8.0 built from source (`ci/Dockerfile.daos-libs`, 23 MB of
    headers and libraries): `ci/Dockerfile.ubuntu-check` builds
    `libplugin_DAOS.so` and the three tests, 312/312 targets, every DAOS
    library resolved. Building the image took three fixes, recorded in its
    comments: SCons's minimal PATH hides venv tools (meson, uv), and the man
    page step runs SPDK-linked tools that die with SIGILL on a CPU without AVX2.
- Not verified: throughput on verbs and multiple ranks with this branch
  (34.17 GB/s was measured with the same backend code on NIXL e77af99, DAOS
  2.9.100); running the Ubuntu-built plugin against a DAOS server.

## Order of submission

1. **Issue first.** Upstream CONTRIBUTING asks that a large feature agree on its
   design in an issue before the PR. `ISSUE-daos-backend.md` → **posted as #2361**
   (five questions: DAOS client in CI, form of the runtime tests, configuration,
   whether to include VRAM_SEG, the copyright line, since upstream
   copyright-check requires an approved copyright holder).
2. Adjust the branch to what is agreed.
3. Push the branch to the fork `hgichon/nixl` (where PR #2246 came from) and open
   the PR with `PR-daos-backend.md` as the body. Commits need a DCO sign-off
   (`Signed-off-by: Kyeongpyo Kim <hgichon@gmail.com>`).
4. When upstream main moves, rebase `feat/daos-backend` and regenerate the two
   patches in this directory.

Upstream says review usually takes one to two weeks and two to four rounds.
