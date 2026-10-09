<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# Contributing

Thanks for looking. Read the first two sections before you spend time on a
change: the repository is mirrored, and the plugin is on its way into NIXL itself.

## Where this repository lives

The upstream is a **GitLab instance inside Gluesys**. GitHub
(`gluesys/nixl-daos`) is a **push mirror** of it, refreshed within minutes.

A commit pushed to GitHub is overwritten by the next mirror run, and a pull
request opened on GitHub cannot be merged there. This is how the mirror is
configured, not a policy about outside contributions.

- **Bug report, question or design proposal**: open a GitHub issue.
- **A patch**: open a GitHub pull request anyway. We read and discuss it there,
  carry the commits to the internal upstream with your authorship kept, and
  close the PR as merged-via-mirror once the mirror brings it back.
- If the change is large, open an issue first.

## Where the plugin is going

The plugin is being proposed to NIXL as an in-tree backend; the RFC and PR drafts
are in [`upstream/`](upstream/). Once it is merged there, changes to the plugin
itself belong in [ai-dynamo/nixl](https://github.com/ai-dynamo/nixl) and follow
its CONTRIBUTING.
Until then, `src/plugins/daos/` and `test/unit/plugins/daos/` here are the
source, and `upstream/` holds the commit as it will be submitted.

So a plugin change here should already meet NIXL's rules:

- Format C++ with NIXL's `.clang-format` (clang-format 19).
- Keep the files buildable inside a NIXL tree: `upstream/apply-integration.sh`
  puts them there and `ci/build.sh` builds NIXL with the plugin (`-Werror`).

## Before you send it

The CI (`.gitlab-ci.yml`) runs what needs neither a DAOS cluster nor a GPU:

```bash
find . -path ./.git -prune -o -name '*.sh' -print | xargs -r -n1 bash -n
find . -path ./.git -prune -o -name '*.py' -print | xargs -r -n50 python3 -m py_compile
```

and a license check: every `.py`, `.sh`, `.c`, `.cpp`, `.h`, `.go` and
`Dockerfile*` must carry

```
SPDX-License-Identifier: Apache-2.0
Copyright 2026 Gluesys Co., Ltd.
```

in its first five lines (your own copyright line instead if the file is
substantially yours).

Two more checks run outside CI because they need the DAOS client image:

```bash
scripts/syntax-check.sh ../nixl <daos-client-image>   # compile-only, no meson
ci/build.sh ../nixl <daos-client-image>               # full NIXL + plugin build
```

## What CI cannot check

The tests in `test/unit/plugins/daos/` need a live DAOS pool and a running
`daos_agent`; `test_gpu` also needs a GPU and a DAOS client with GPU-direct
support. We deliberately did not add a job that skips itself when the cluster is
absent: a green pipeline that checked nothing is worse than no pipeline.

If your change touches a path only a cluster exercises, say so in the PR and
describe what you ran. We run `ci/e2e-testbed.sh` against our testbed before
landing it. You are not expected to own hardware to contribute.

## Claims about performance

If a change comes with a number, give the conditions with it: the transport
(`ofi+verbs;ofi_rxm`, `ofi+tcp`, ...), the number of DAOS ranks, object class,
descriptor count and size, `NIXL_DAOS_THREADS`, and how many runs the number
came from. A measured range beats a single best result. The measurements in
[`doc/measurements/`](doc/measurements/) show the expected level of detail,
including the conclusions they later retracted.

## Commits

- One logical change per commit. Say **why**; the diff already says what.
- Sign off (`git commit -s`): the
  [Developer Certificate of Origin](https://developercertificate.org/). NIXL
  requires it too. There is no CLA.
- Use a real name and a working email in `Author:`.
- Contributions are accepted under Apache-2.0 (see `LICENSE` §5).

## Layout

See the table in [README.md](README.md#layout).

## Contact

Open an issue for anything public. For a security report, do not use the issue
tracker; see [SECURITY.md](SECURITY.md).
