<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# NIXL DAOS backend measurements (2026-09-12, client-5)

*Translated from [gluesys/lmcache-daos `doc/NIXL-DAOS-MEASUREMENT.md`](https://github.com/gluesys/lmcache-daos/blob/main/doc/NIXL-DAOS-MEASUREMENT.md) (Korean original). File paths refer to the lmcache-daos repository unless noted; the NIXL plugin that lived under `nixl/` there is now `src/plugins/daos/` in this repository.*

## Summary

**34.17 GB/s** over 400G verbs. This is with 40 layers folded into one RPC.
Without folding it drops to 14.40 GB/s.

Two things produce this number: **folding** and **concurrency**. While measuring
the two separately, we found that two of this document's initial interpretations
were wrong. The corrections are kept in §4.

## 1. Environment

client-5, Rocky 10.2 / kernel 6.12, gcc 14.3.1, CUDA 13.3, UCX 1.21.0.
DAOS 2.9.100 stockfull, 2 ranks (cell1/cell2), 16 targets, transport `ofi+verbs;ofi_rxm`,
agent domain `mlx5_0`, ConnectX-7 400G RoCE (no PFC).
Pool `attr1`, container `nixlbench` (plain, rd_fac:0).

The load shape matches `tests/obj_latency.c`.
**120 objects × 40 layers × 1 MiB = 4.69 GiB.** The harness is `nixl/tests/bench_nixl.cpp`.
It drives the backend directly, without going through a NIXL agent.

At measurement time client-5 was idle (GPU 0%, 0 containers). However, the `attr1` pool
also holds `discos-gdr-20260912` and `discos-posix-20260912`, created the same day by
another team. We did not check whether they were doing concurrent I/O.

## 2. Folding

`prepXfer` collects descriptors with the same (object, dkey) and turns them into a single
`daos_obj_fetch`. `-f` turns this on and off.

| | Requests | Read | Bandwidth |
|---|---|---|---|
| `fold=1` (40 layers into 1 request) | 120 | **147.3 ms** | **34.17 GB/s** |
| `fold=0` (one request per layer) | 4800 | 349.5 ms | 14.40 GB/s |

Both use 64 threads, inflight 64. The bytes are identical. **2.4×.**

## 3. Concurrency

We separated the two variables by fixing one at a time.

**A. inflight fixed at 64, only threads varied (`fold=0`)**

| Threads | Read | Bandwidth |
|---|---|---|
| 16 | 607.8 ms | 8.28 GB/s |
| 32 | 445.8 ms | 11.29 GB/s |
| 64 | 350.8 ms | 14.35 GB/s |
| 128 | 351.9 ms | 14.30 GB/s |

**B. threads fixed at 64, only inflight varied (`fold=0`)**

| inflight | Read | Bandwidth |
|---|---|---|
| 16 | 630.8 ms | 7.98 GB/s |
| 32 | 453.6 ms | 11.10 GB/s |
| 64 | 349.5 ms | 14.40 GB/s |
| 128 | 348.9 ms | 14.43 GB/s |

**The two tables are the same curve.** With 64 threads but inflight 16 you get 8 GB/s,
and the reverse is the same. Effective concurrency is `min(threads, in-flight requests)`,
and it **flattens at 64.**

So we set the default thread count to 64 (adjustable with `NIXL_DAOS_THREADS`). If the
caller keeps few requests in flight, throughput is bound by the caller's depth regardless
of this value.

`fold=1` also responds to threads, but the range is small: 30.53 GB/s at 16, 34.17 GB/s at 64.
There are only 120 requests, so it cannot use all the concurrency.

## 4. Corrections

We withdraw two interpretations made during measurement. Both were **comparisons with
mismatched concurrency**.

**(1) "Thread creation cost is the bottleneck" — wrong.**

Replacing per-request `std::async` with a thread pool turns 4800 threads into 16 reused ones.
But the result went from 624.7 ms → 616.3 ms, **a 1.3% difference**. The driver was capping
inflight at 16, so either way there were 16 concurrent calls. Creation cost was never the
bottleneck.

We keep the thread pool anyway. The reason is structure, not performance. Creating and
destroying one thread per request puts real pressure on memory and the scheduler even if
the measurement does not show it. Above all, **being able to control concurrency with
`NIXL_DAOS_THREADS` is what made the sweep above possible.**

**(2) "There is no NIXL layer overhead" — wrong.**

This conclusion came from `fold=1` at 161.5 ms being faster than raw libdaos `batch` at 204 ms.
That raw measurement had 16 threads running synchronous calls sequentially, while this side
is pipelined, so the comparison does not hold.

With matched shape and thread count:

| Path | 4800 requests, 16 threads |
|---|---|
| raw libdaos (`tests/obj_latency.c` `sep`) | 245 ms |
| NIXL backend (`fold=0`) | 607.8 ms |

The NIXL path spends **about 0.076 ms more per request**. Candidates: `dynamic_cast` in
`prepXfer`, `std::map` insertion, 5 vector allocations per group, and the mutex and
condition variable in `postXfer`.

This is also what the folding effect really is.

| | Requests | Estimated overhead | Share of total |
|---|---|---|---|
| `fold=1` | 120 | 9 ms | 5% of 165 ms |
| `fold=0` | 4800 | 363 ms | 60% of 608 ms |

However, this 0.076 ms **is a comparison across different harnesses, so it is not a
controlled value.** Measuring it properly requires swapping only the backend under the same
driver. We leave it unmeasured.

We also correct the folding factor. In the comparison with mismatched concurrency we saw 3.9×;
with matched concurrency it is **2.4×**.

## 5. Correctness

After switching to the thread pool, both programs in `nixl/tests/` still pass. In particular,
we confirmed that reading a key that was never written returns `NIXL_ERR_NOT_FOUND`. In that
case DAOS answers with success and `iod_size 0`. If passed through as is, a zero-filled buffer
would be reported as a hit.

## 6. Next

- **Per-request allocation and synchronization cost.** Handle reuse, bypassing `std::map`
  when there is only one group, pre-reserving vectors. Targets the 0.076 ms in §4(2).
- **Controlled overhead measurement.** Measuring the POSIX backend and the DAOS backend under
  the same driver separates the NIXL layer cost.
- **`VRAM_SEG`.** `daos_obj_fetch_gpu` is in the `theodore/b_cufile` client.
  client-5 has that library at `/opt/daos-gds-gpu`, so it is possible here.
- **Going through a NIXL agent.** For now we only drive the backend directly.

## Reproduction

```bash
# client-5, via cell1
cd /home/nixl-dev/t
NIXL_DAOS_THREADS=64 ./bench_nixl -p attr1 -c nixlbench \
    -o 120 -l 40 -s 1048576 -i 64 -r 2 -f 1
```

Build steps are in `nixl/README.md`. client-5 has no meson or ninja, so we set them up with
`ensurepip` → pip. `nvcc` must be on PATH. DAOS is at `/var/daos-stockfull`, so the plugin's
`meson.build` searches for the prefix.
