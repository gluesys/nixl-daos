<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# Upstream (ai-dynamo/nixl) submission

| file | contents |
|---|---|
| `0001-plugins-add-DAOS-storage-backend.patch` | one commit on upstream main (DCO signed off): the whole change the PR will carry |
| `integration.patch` | the meson wiring part of that commit (`meson.build`, `meson_options.txt`, `src/plugins/meson.build`, `test/unit/plugins/meson.build`) |
| `apply-integration.sh` | puts this repository's plugin and tests into a NIXL tree and applies `integration.patch`; used by CI (`ci/build.sh`) |
| `ISSUE-daos-backend.md` | the upstream design-discussion issue. **Posted 2026-10-08, withdrawn 2026-10-09 with no replies; goes up again as a new issue once the docs it points at are ready** |
| `PR-daos-backend.md` | draft PR body (upstream template What/Why/How; not posted) |
| `ci/Dockerfile.daos-libs` | DAOS client headers and libraries for Ubuntu as a small data image, the way upstream's `Dockerfile.infinia-libs` supplies Infinia (answer (a) to question 1 of the issue) |
| `ci/Dockerfile.ubuntu-check` | builds NIXL + the DAOS plugin and tests on Ubuntu 24.04 from that image, with upstream CI's build settings |

## Base and verification (2026-10-08)

- Base: ai-dynamo/nixl main `44c1b56`. Local branch `feat/daos-backend` (worktree
  `../nixl-wt-daos`, commit `6fad003`).
- Form: follows upstream CONTRIBUTING and the most recent external plugin
  (INFINIA, DDN), minus what its review rejected: `-Ddaos_path` only, no
  `disable_*` option (`-Ddisable_plugins=DAOS` covers it); skipped with a
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
    `-Ddisable_plugins=DAOS` leaves it out silently.
  - With DAOS v2.8.0 built from source (`ci/Dockerfile.daos-libs`, 23 MB of
    headers and libraries): `ci/Dockerfile.ubuntu-check` builds
    `libplugin_DAOS.so` and the three tests, 312/312 targets, every DAOS
    library resolved. Building the image took three fixes, recorded in its
    comments: SCons's minimal PATH hides venv tools (meson, uv), and the man
    page step runs SPDK-linked tools that die with SIGILL on a CPU without AVX2.
- Not verified: throughput on verbs and multiple ranks with this branch
  (34.17 GB/s was measured with the same backend code on NIXL e77af99, DAOS
  2.9.100); running the Ubuntu-built plugin against a DAOS server.

## Prepared against the INFINIA review (2026-10-08)

The review of the INFINIA plugin (ai-dynamo/nixl#1569, 175 inline comments, about
six weeks) was checked point by point against this plugin. Of 38 points, 21
already held here; these were changed before submitting:

- SPDX: an NVIDIA line above the Gluesys line, as INFINIA and the libfabric
  plugin (Amazon) do; upstream `copyright-check.sh` passes.
- The constructor sets `initErr` when `daos_init()` fails, so the agent refuses
  the backend instead of using a broken one.
- `NIXL_DAOS_THREADS` and `NIXL_DAOS_EQ_TIMEOUT` are read through
  `nixl::config` (environment or config file). The timeout is whole seconds:
  the config layer has no floating-point type.
- No `disable_daos_backend` option; the review asked INFINIA to drop its own
  and use `-Ddisable_plugins`.
- Static builds register the plugin in `src/core`.
- `[[nodiscard]] ... noexcept` on the capability methods; one supported-memory
  list shared by the plugin declaration and the engine.
- `prepXfer()` rejects anything but a memory local list and a `FILE_SEG`
  remote list.
- Comments that contradicted the code were fixed; names follow
  `docs/CodeStyle.md`.

Checked after the change: clang-format 19; Ubuntu 24.04 builds with DAOS
(shared 312/312, static 311/311 with the plugin registered in `libnixl.so`),
without DAOS (skipped with a warning), and with `-Ddisable_plugins=DAOS`; on
CI lane B (DAOS 2.8.0, 1 rank, `ofi+tcp`) the three tests pass, plus
`test_xfer` with `NIXL_DAOS_EQ_TIMEOUT=0` and with a malformed
`NIXL_DAOS_THREADS` (warned, default used).

## Order of submission

1. **Issue first.** Upstream CONTRIBUTING asks that a large feature agree on its
   design in an issue before the PR. `ISSUE-daos-backend.md` → **posted 2026-10-08
   and withdrawn the next day with no replies**, to finish tidying the
   documentation it links to. Re-post it as a *new* issue rather than reopening
   the old one, then put the number back in this file and in the repository
   README and CONTRIBUTING. Four questions remain: DAOS client in CI, form of the
   runtime tests, configuration, whether to include VRAM_SEG. The copyright
   question is settled — NVIDIA line next to ours, as the INFINIA plugin does.
2. Adjust the branch to what is agreed.
3. Push the branch to the fork `hgichon/nixl` (where PR #2246 came from) and open
   the PR with `PR-daos-backend.md` as the body. Commits need a DCO sign-off
   (`Signed-off-by: Kyeongpyo Kim <hgichon@gmail.com>`).
4. When upstream main moves, rebase `feat/daos-backend` and regenerate the two
   patches in this directory.

Upstream says review usually takes one to two weeks and two to four rounds.
