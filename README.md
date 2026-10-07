<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# nixl-daos

A [DAOS](https://github.com/daos-stack/daos) storage backend for
[NIXL](https://github.com/ai-dynamo/nixl), the NVIDIA Inference Xfer Library. It
moves data between local memory and DAOS objects through the DAOS object API
(dkey/akey), not the DFS file layer, and is meant for KV cache offload from
Dynamo/KVBM and the LMCache NIXL backend.

> **Out of tree, on its way upstream.** NIXL has no DAOS backend today. The design
> is proposed in [ai-dynamo/nixl#2361](https://github.com/ai-dynamo/nixl/issues/2361);
> the PR follows once it is agreed. This repository holds the plugin until then,
> plus its container build and testbed scripts.

## What has been measured

- **34.17 GB/s** reading 4.69 GiB (120 objects x 40 layers x 1 MiB) from 2 DAOS 2.8
  ranks over 400G verbs. Without folding the 40 layers into one RPC: 14.40 GB/s.
- DFS costs 0.63 ms fixed per object, the object API 0.0137 ms, on the same 4800
  reads of 1 MiB.
- With `daos_server` killed mid-read, the event-queue deadline returns in 8 s with
  every thread reclaimed; a blocking call was still stuck at 120 s.
- On NIXL main, against an upstream DAOS 2.8.0-6 server (1 rank, `ofi+tcp`), the
  `reg`, `xfer` and `agent` tests pass.

The design, the descriptor mapping and the method behind each number are in
[`src/plugins/daos/README.md`](src/plugins/daos/README.md). The full measurement
reports (in Korean) are in
[gluesys/lmcache-daos `doc/`](https://github.com/gluesys/lmcache-daos/tree/main/doc).

## Layout

| path | contents |
|---|---|
| `src/plugins/daos/` | the plugin: `nixlDaosEngine`, meson build, user documentation |
| `test/unit/plugins/daos/` | programs that need a live DAOS pool: `test_reg`, `test_xfer`, `test_agent`, plus `test_gpu` and `bench_nixl` (not part of the upstream submission) |
| `upstream/` | the upstream submission: `integration.patch` and `apply-integration.sh` wire the plugin into a NIXL source tree; `0001-*.patch` is the commit as it will be submitted; issue and PR drafts |
| `ci/build.sh` | builds NIXL + the plugin inside a DAOS client image and produces the runtime image `nixl-daos:dev` (`images/Dockerfile.runtime`) |
| `ci/e2e-testbed.sh` | runs the tests from that image against an existing DAOS pool on a client host |
| `doc/adr/` | design decisions specific to this repository |

## Build and test

```bash
# NIXL source checkout next to this repository; DAOS client image with daos-devel
ci/build.sh ../nixl registry.example/daos-client:2.8.0 4

# against a live pool, from a host running daos_agent
ci/e2e-testbed.sh root@client-host <pool> <daos-system-name> nixl-daos:dev
```

Inside a container, reaching the host `daos_agent` socket needs
`--security-opt label=disable` on SELinux hosts.

## Conventions

Shared with the other Gluesys DAOS repositories
([daos-operator](https://github.com/gluesys/daos-operator),
[daos-csi](https://github.com/gluesys/daos-csi),
[daos-images](https://github.com/gluesys/daos-images)):
fixes go upstream first (daos-stack, ai-dynamo/nixl, LMCache), and destructive DAOS
operations (format, wipe) are never automated.

## License

Apache-2.0. See [LICENSE](LICENSE).
