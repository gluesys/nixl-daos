<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->
# vLLM + LMCache through the NIXL DAOS backend

LMCache's NIXL storage backend stores and reads vLLM's KV cache in DAOS through
this plugin. Every repeated prompt was a full hit read back from DAOS.

## What it takes

| piece | change |
|---|---|
| plugin | `pool` and `container` backend parameters: the target for descriptors with an empty `metaInfo` |
| LMCache 0.5.5 | [`lmcache-0.5.5-nixl-daos-backend.patch`](lmcache-0.5.5-nixl-daos-backend.patch): accept `DAOS` as a NIXL backend name and treat it as a file backend |
| image | [`Dockerfile`](Dockerfile): the serving image `vllm-lmcache-daos` plus NIXL built from source with the plugin, for python3.12 |

LMCache's file backends register one descriptor per slot as `(0, size, fd, "")`:
an empty `metaInfo` and the slot's fd as `devId`. The plugin maps that to an object
in the default container, with the object id derived from the fd. Two processes that
share a container therefore share object ids; give each process its own container.

LMCache configuration (in-process mode, CPU staging buffer, no CPU tier, so a hit
can only come from DAOS):

```yaml
chunk_size: 256
local_cpu: false
max_local_cpu_size: 2.0
nixl_buffer_device: cpu
extra_config:
  enable_nixl_storage: true
  nixl_backend: DAOS
  nixl_backend_params: {pool: <pool>, container: <container>}
  nixl_path: /tmp/nixl-slots   # LMCache opens one empty local file per slot for its fd
  nixl_pool_size: 512
```

The pod also needs a `daos_agent` (a sidecar with the system's agent config) and
`DAOS_AGENT_DRPC_DIR` pointing at its socket.

## Result (2026-10-29)

Lane A of the DAOS Kubernetes CI: DAOS 2.8.0, 4 ranks on kdev (`/dev/sdb`), ib0,
existing pool `e2epool` (24 GB, rd_fac 0), container `nixlvllm`. vLLM 0.30.0 with
Qwen2.5-1.5B-Instruct on an NVIDIA A2, `--no-enable-prefix-caching`. Load:
`lmcache-daos/tests/vllm_reboot_ttft.py --prompts 16 --words 2000 --secs 300
--fresh-every 50` (each prompt about 2,800 tokens, `max_tokens=1`, so request time
is TTFT).

| request | count | failed | TTFT p50 | p90 |
|---|---|---|---|---|
| first time (stored) | 16 | 0 | 0.743 s | 0.761 s |
| repeated (hit) | 1,634 | 0 | **0.166 s** | 0.199 s |
| never seen (recomputed) | 33 | 0 | 0.749 s | 0.755 s |

- LMCache logged 1,837 retrievals (the run above plus a 30 s probe), every one
  "Retrieved N out of N required tokens": no partial hit, no fallback to recompute.
  One retrieval of about 75 MB took 96 ms at p50 (p90 113 ms, 65-313 ms).
- During a 30 s repeat-only probe the DAOS engines' fetch count rose by 4,678
  while the update count did not change: the hits were reads from DAOS.
- 1,633 of 1,634 repeats finished under 0.3 s.

For comparison, the baseline this e2e was set against: the lmcache-daos in-process
connector reads a cold hit in 76-211 ms.

Not covered: the slot index lives in the LMCache process, so the cache is not found
again after a restart, and the run used a CPU staging buffer only (no GPU-direct).
