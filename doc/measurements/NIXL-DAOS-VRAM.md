<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# VRAM_SEG measurements — the GPU direct path works but is slow (2026-09-13, client-5)

*Translated from [gluesys/lmcache-daos `doc/NIXL-DAOS-VRAM.md`](https://github.com/gluesys/lmcache-daos/blob/main/doc/NIXL-DAOS-VRAM.md) (Korean original). File paths refer to the lmcache-daos repository unless noted; the NIXL plugin that lived under `nixl/` there is now `src/plugins/daos/` in this repository.*

## Summary

We added a path that reads and writes DAOS objects directly to and from GPU memory.
**Correctness passes.** We wrote a payload generated on the device to DAOS, read it back
into a different GPU buffer, compared on the device side, and everything matched.

**But it is 3.4× slower than going through host memory.** The cause is not our code.
CaRT re-registers the GPU buffer on every transfer, and that cost is charged not per byte
but at **about 0.43 ms per sgl entry**.

A means to avoid this exists by design (`daos_mem_attr_t::ma_rkey`), but
**we cannot use it in our structure.** §4 gives the reason.

Conclusion: `VRAM_SEG` stays **disabled by default** and is marked experimental.

## 1. Environment

client-5. DAOS client is `/opt/daos-gds-gpu` (unmerged `theodore/b_cufile`),
transport stack is `/opt/ofi-cuda` (libfabric built with CUDA) + patched mercury and UCX,
`D_MEM_DEVICE=1`, `D_GPU_DIRECT=1`. H100 NVL, ConnectX-7 400G RoCE.
Pool `attr1`, container `nixlgpu`.

client-5 has a complete cuFile stack (`nvidia_fs` loaded, `/dev/nvidia-fs*`,
`libcufile.so`, `/etc/cufile.json`). The development host cxl2 does not, so this work
is only possible on client-5.

## 2. Enabled only at build time

`daos_obj_fetch_gpu()` / `daos_obj_update_gpu()` exist only on the unmerged branch.
Advertising `VRAM_SEG` against a stock client would turn a clean "not supported" into a
link error or something worse. So it enters the capability list only when meson checks
that the symbols exist and defines `NIXL_DAOS_HAVE_GPU`.

```
DAOS prefix: /var/daos-stockfull
DAOS GPU-direct: not in this client (VRAM_SEG disabled)     ← default

DAOS_PREFIX=/opt/daos-gds-gpu
DAOS GPU-direct: available (VRAM_SEG enabled)               ← explicit choice
```

`DAOS_PREFIX` is an environment variable because two clients coexist on one host.
Which one to use is a deployment decision, not something to guess from search order.

## 3. Measurements

120 objects × 40 layers × 1 MiB = 4.69 GiB, inflight 64, 64 threads.
Only the staging buffer changes, DRAM ↔ VRAM.

| | Read | Bandwidth |
|---|---|---|
| DRAM, folded | 154.3 ms | **32.61 GB/s** |
| DRAM, not folded | 349.6 ms | 14.39 GB/s |
| VRAM, folded | 1994.7 ms | **2.52 GB/s** |
| VRAM, not folded | 1884.2 ms | 2.67 GB/s |

**Folding has no effect on VRAM.** DRAM gets 2.3× faster when folded; VRAM actually gets
slightly slower. Folding reduces the RPC count from 4800 → 120, so if it has no effect,
the cost is not per RPC.

### The cost is per sgl entry

We kept total bytes fixed at 4.69 GiB and changed only the entry count.

| sgl entries | VRAM | DRAM |
|---|---|---|
| 4800 (40 × 1 MiB) | 2080.9 ms / 2.42 GB/s | 150.5 ms / 33.45 GB/s |
| 1200 (10 × 4 MiB) | **516.0 ms / 9.75 GB/s** | 146.6 ms / 34.33 GB/s |
| 300 (10 × 16 MiB) | 637.9 ms / 7.89 GB/s | 625.2 ms / 8.05 GB/s |

4800 → 1200 is a 4× reduction in entries, and time dropped **4.03×**. A cleaner proportion
is not possible. **About 0.43 ms per entry.** Over the same range DRAM does not change,
150.5 → 146.6 ms.

Both collapsing at 300 entries is a separate phenomenon. With 16 MiB layers, the extent of a
single akey gets large. It is the same pattern as the `one` arm of `tests/obj_latency.c`
being the slowest at 571 ms. **The optimum is in the middle.**

## 4. `ma_rkey` is not a knob we can use

If `daos_mem_attr_t::ma_rkey` is empty, CaRT registers the GPU buffer with `fi_mr_reg()`
every time. The source states this situation directly
(`daos-gds/src/client/cufile/cufile_plugin.c`).

> If ma_rkey is empty, CaRT falls back to normal fi_mr_reg() with FI_HMEM_CUDA
> — still correct, just redundant registration.

The measured 0.43 ms is that "redundant registration".

If it is filled, the path goes to `crt_bulk_import_rkey()`, but **we have no value to put in.**

```c
d_iov_set(&mem_attr.ma_rkey, (void *)rdma_info->desc_str, rdma_info->desc_len);
```

`rdma_info` is a `cufileRDMAInfo_t`, and it is **what the cuFile driver passes to the plugin
callback when it performs IO through a registered file handle**. We do not get it by calling
`cuFileBufRegister()` ourselves. That is, this path holds only when the application does IO
through the cuFile API and DAOS runs underneath it as a plugin.

Our NIXL backend goes the other way. We call DAOS directly, so there is no place for cuFile
to step in.

There is one more transport-specific constraint. `src/cart/crt_bulk.c` refuses raw rkey import
entirely on UCX (a registration made by nvidia-fs cannot be brought into the UCX path). Only on
verbs does it go to `crt_bulk_import_rkey()`. We use verbs, so this condition passes, but the
fact remains that there is one more constraint.

## 5. The MR cache is also blocked

Covering re-registration with a cache is also closed off. As recorded in `gpudirect/README.md`,
mercury `na_ofi` forces `FI_MR_CACHE_MAX_COUNT=0`, and overriding it with an environment variable
has no effect. In this run too, `FI_MR_*` were left unset at their defaults.

## 6. Verdict

- `VRAM_SEG` **passes correctness and falls short on performance**. It stays disabled by default.
- For KV cache use today, **going through DRAM is 3.4× faster.** The common belief that GPU direct
  is better does not hold for this DAOS implementation.
- However, this benchmark ends at the staging buffer. In practice the DRAM arm must copy once more
  from there to the GPU, and that cost is not captured. This is a **measurement that favors DRAM**,
  so 3.4× is closer to an upper bound than a lower bound. Even so, it is not large enough to cover
  0.43 ms per entry.

## 7. Candidate for upstream report

CaRT does not cache GPU buffer registrations. `ma_rkey` is open only on the cuFile plugin path,
and a client that uses the object API directly has no way to avoid re-registration. This can be
given as feedback on the `theodore/b_cufile` draft.

## Reproduction

```bash
# client-5. Requires the GDS transport stack
G=/opt/daos-gds-gpu
PRE=$(ls -d $G/prereq/release/*/lib64 | grep -v '/ofi/' | tr '\n' ':')
export LD_LIBRARY_PATH=/opt/ofi-cuda/lib64:$G/lib64:${PRE}/usr/local/cuda/lib64:...
export D_MEM_DEVICE=1 D_GPU_DIRECT=1
ulimit -l unlimited; ulimit -n 65536

./test_gpu   attr1 nixlgpu                       # correctness
./bench_gpu  -p attr1 -c nixlgpu -o 120 -l 40 -s 1048576 -i 64 -r 2 -f 1 -g 1
```

The plugin must be built with `DAOS_PREFIX=/opt/daos-gds-gpu` for `VRAM_SEG` to be enabled.
