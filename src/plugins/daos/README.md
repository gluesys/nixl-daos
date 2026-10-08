<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-FileCopyrightText: Copyright (c) 2026 Gluesys Co., Ltd.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# NIXL DAOS Plugin

## Overview

A storage backend for [DAOS](https://github.com/daos-stack/daos), the
distributed asynchronous object store. It moves data between local memory and
DAOS objects with the DAOS **object API** (dkey/akey), not the DFS file layer.

- **Segments:** `DRAM_SEG` (local) and `FILE_SEG` (DAOS objects). `VRAM_SEG` is
  advertised only when the DAOS client exports GPU-direct entry points (see
  [GPU memory](#gpu-memory)).
- **Local transfers only:** `supportsLocal()` is true, `supportsRemote()` false.
- **Throughput:** 34.17 GB/s reading 4.69 GiB (120 objects x 40 layers x 1 MiB)
  from 2 DAOS ranks over 400G verbs ([measurement][meas]).

### Why the object API and not DFS

The same 4800 reads of 1 MiB, measured both ways ([measurement][layer]):

| path | fixed cost per object | marginal |
|---|---|---|
| DFS (`dfs_sys_read`, flat namespace) | 0.63 ms | 0.067 ms/MiB |
| object API (dkey/akey) | **0.0137 ms** | 0.0385 ms/MiB |

At 1 MiB objects DFS spends about 90% of the time on overhead the object API
does not pay. The object API also folds a whole descriptor list into one
`daos_obj_fetch()` through its iod array, which is the shape `prepXfer()` hands
the backend.

## Dependencies

- A DAOS client: headers (`daos.h`) and `libdaos`, from the `daos-devel`
  package or a source build. Tested with DAOS 2.8.
- A running `daos_agent` on the host, and a DAOS pool and container the process
  may access.

## Build Instructions

The plugin is built when meson finds the DAOS client and skipped with a warning
when it does not.

```bash
# DAOS client in the default search path (e.g. the daos-devel package)
meson setup build
ninja -C build

# DAOS client under a prefix
meson setup build -Ddaos_path=/opt/daos

# Fail instead of skipping when DAOS is missing
meson setup build -Denable_plugins=DAOS

# Never build it
meson setup build -Ddisable_plugins=DAOS
```

The plugin library is `libplugin_DAOS.so` under the NIXL plugin directory.

## API Reference

`nixlDaosEngine` (`daos_backend.h`) implements `nixlBackendEngine`.

### Descriptor mapping

| field | meaning |
|---|---|
| `metaInfo` | `"pool/container"`, or `"pool/container/<hi>.<lo>"` for an explicit object id |
| `devId` | the caller's key for the object; the object id is derived from it when `metaInfo` has two fields |
| `addr` | offset within the object |
| `len` | bytes |

The object id is derived from `devId` so that the same key names the same
object across process restarts; otherwise a cache could not find what it
stored.

Inside an object, an offset maps to

```
dkey = addr / 64 MiB      akey = addr % 64 MiB
```

Descriptors within one 64 MiB span share a dkey, land on one target and fold
into a single RPC; separate spans spread across targets. On the same 4.69 GiB,
40 akeys under one dkey took 204 ms, one akey per RPC 245 ms, and a single
40 MiB akey extent 571 ms.

### Configuration

| setting | default | effect |
|---|---|---|
| `NIXL_DAOS_THREADS` | 64 | size of the worker pool (1..512). Throughput flattens at 64 on 400G verbs |
| `NIXL_DAOS_EQ_TIMEOUT` | 60 | whole seconds a request may wait on a DAOS event queue before it fails; `0` blocks |

Both are read through NIXL's configuration, so they can be set as environment
variables or in the NIXL configuration file.

The event-queue deadline exists because a blocking DAOS call whose engine has
died does not return. With `daos_server` killed mid-read, the blocking path was
still stuck at 120 s with 65 threads; an 8 s deadline returned in 8 s with every
thread reclaimed ([failure modes][fail]). It costs nothing measurable: 30.64
against 31.04 GB/s over four runs, ranges overlapping.

### GPU memory

`VRAM_SEG` needs `daos_obj_fetch_gpu()`, which only a DAOS client built with
GPU-direct support exports; meson checks for the symbol and advertises
`VRAM_SEG` only when it is present. It works -- device-to-device round trips
are bit-identical -- but is currently about 3.4x slower than staging through
host memory, because the transport re-registers the GPU buffer on every
transfer ([details][vram]).

## Example Usage

```cpp
#include "nixl.h"

nixlAgent agent("app", nixlAgentConfig());
nixlBackendH *daos = nullptr;
agent.createBackend("DAOS", nixl_b_params_t(), daos);

nixl_opt_args_t ext;
ext.backends.push_back(daos);

// One DAOS object in pool "p", container "c", named by devId 42. Its size is
// not fixed at registration: transfers address it by offset.
nixl_reg_dlist_t objs(FILE_SEG);
nixlBlobDesc obj(0, 0, /*devId=*/42);
obj.metaInfo = "p/c";
objs.addDesc(obj);
agent.registerMem(objs, &ext);

std::vector<char> buf(1 << 20);
nixl_reg_dlist_t dram(DRAM_SEG);
dram.addDesc(nixlBlobDesc((uintptr_t)buf.data(), buf.size(), 0));
agent.registerMem(dram, &ext);

// Write the buffer to offset 0 of the object and wait for it.
nixl_xfer_dlist_t local(DRAM_SEG), remote(FILE_SEG);
local.addDesc(nixlBasicDesc((uintptr_t)buf.data(), buf.size(), 0));
remote.addDesc(nixlBasicDesc(0, buf.size(), 42));

nixlXferReqH *req = nullptr;
agent.createXferReq(NIXL_WRITE, local, remote, "app", req, &ext);
nixl_status_t st = agent.postXferReq(req);
while (st == NIXL_IN_PROG)
    st = agent.getXferStatus(req);
agent.releaseXferReq(req);
```

`test/unit/plugins/daos/test_agent.cpp` is the complete, compiled version.

## Testing

The programs in `test/unit/plugins/daos/` need a reachable pool and container,
so they are built and installed but not registered with `meson test`.

```bash
nixl_daos_test_reg   <pool> <container>   # register/deregister, object id stability
nixl_daos_test_xfer  <pool> <container>   # write/read round trip, integrity, miss detection
NIXL_PLUGIN_DIR=<build>/src/plugins/daos \
nixl_daos_test_agent <pool> <container>   # the same through a real nixlAgent
```

`test_xfer` writes a self-describing payload -- every 8-byte word encodes its
own descriptor and offset -- so a region that comes back wrong names where it
actually came from.

## Limitations

- Local transfers only; no remote (agent-to-agent) path.
- About 0.076 ms of per-request overhead in the NIXL path (`dynamic_cast`, a
  `std::map` in `prepXfer()`, per-group vector allocations). Folding hides it
  (5% of a folded transfer, 60% of an unfolded one).
- The 64 MiB dkey span is fixed; it should become a plugin parameter once there
  is a second workload to tune it against.

[meas]: https://github.com/gluesys/nixl-daos/blob/main/doc/measurements/NIXL-DAOS-MEASUREMENT.md
[layer]: https://github.com/gluesys/nixl-daos/blob/main/doc/measurements/LAYERWISE-MEASUREMENT.md
[fail]: https://github.com/gluesys/nixl-daos/blob/main/doc/measurements/FAILURE-MODES.md
[vram]: https://github.com/gluesys/nixl-daos/blob/main/doc/measurements/NIXL-DAOS-VRAM.md
