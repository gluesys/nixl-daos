<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# LMCache layerwise mode measurement (2026-09-11, client-5)

*Translated from [gluesys/lmcache-daos `doc/LAYERWISE-MEASUREMENT.md`](https://github.com/gluesys/lmcache-daos/blob/main/doc/LAYERWISE-MEASUREMENT.md) (Korean original). File paths refer to the lmcache-daos repository unless noted; the NIXL plugin that lived under `nixl/` there is now `src/plugins/daos/` in this repository.*

## Summary

`use_layerwise: True` works with our `DaosConnector` **without code changes.** The hit rate is also 100%.
But at the same `chunk_size`, hit TTFT is **11.5x slower.** The bytes are the same. Only the object
count grows by the number of layers (40).

The cause is not bandwidth. It is a **fixed cost of about 0.63 ms per object**. We varied the object
size three ways, fitted a line, and separated the two terms. We also confirmed that changing the
worker count does not change it.

Layerwise wins in one case. **Cold-miss TTFT is halved.** Stores overlap with per-layer compute.

## Environment

client-5, Qwen3-14B (40 layers, 8 KV heads, head_dim 128, bf16), `--max-model-len 32768`,
`--enforce-eager --no-enable-prefix-caching`, DAOS 2.9.100 stockfull, pool `attr1`,
container `kvlmc5` (S16, chunk 4 MiB, rd_fac:0), transport `ofi+verbs;ofi_rxm`,
agent domain `mlx5_0`, `local_cpu: false`, IO thread pool 16 (when not stated otherwise).
The harness is `bench/sweep2.py` (fixed token sequence, sends `prompt_token_ids`).

At `chunk_size: 256`, one layer's chunk is exactly 1 MiB
(256 tokens × 8 heads × 128 dim × 2(K,V) × 2 B). All layers together are 40 MiB.

## Hit TTFT (ms)

| ctx | base/256 | base/1024 | lw/256 | lw/1024 | lw/4096 |
|---|---|---|---|---|---|
| 8192 | 166 | **145** | 982 | 340 | 270 |
| 16384 | 176 | **159** | 1843 | 595 | 330 |
| 30720 | 290 | **269** | 3343 | 1075 | 464 |

`lw/256` failed the `hit < miss/2` gate of `sweep2.py` at all three contexts and was
marked INVALID. The cache did hit (the log shows `Retrieved 30720 out of 30720`).
The hit just did not cut time by even 2x compared with recompute.

## Miss TTFT (ms) — here layerwise wins

| ctx | base/256 | base/1024 | lw/256 | lw/1024 | lw/4096 |
|---|---|---|---|---|---|
| 8192 | 883 | 1391 | 1275 | 785 | **620** |
| 16384 | 2379 | 2854 | 2344 | 1419 | **1338** |
| 30720 | 5579 | 5756 | 4462 | 2976 | **2837** |

At 30720, `base/1024` 5756 ms → `lw/4096` 2837 ms is **2.03x** faster.
The reason is that stores happen per layer and overlap with forward compute. Non-layerwise
stores everything at once after forward finishes, so the first token is delayed by that much.

## Separating the fixed cost

At 30720, effective time per object = wall-clock time / object count (value includes 16 workers).

| arm | objects | object size | wall clock | per object |
|---|---|---|---|---|
| lw/256 | 4800 | 1 MiB | 3343 ms | 0.696 ms |
| lw/1024 | 1200 | 4 MiB | 1075 ms | 0.896 ms |
| lw/4096 | 280 | 16 MiB | 464 ms | 1.657 ms |

The three points fit a line well.

```
t_eff(size) ≈ 0.63 ms + 0.067 ms/MiB
```

Fitting with the 1 MiB and 4 MiB points and predicting 16 MiB gives 1.70 ms. The measured value is 1.657 ms.

The slope converts to 15.7 GB/s of bandwidth. **The ability to move data is normal.**
The problem is the intercept. For a 1 MiB object, **90% of the time is fixed cost**.

| object size | fixed | transfer | fixed share |
|---|---|---|---|
| 1 MiB | 0.63 ms | 0.07 ms | 90% |
| 4 MiB | 0.63 ms | 0.27 ms | 70% |
| 16 MiB | 0.63 ms | 1.07 ms | 37% |

## More workers make it worse

We measured `lw/256` with IO thread pool 16 and with 128.

| ctx | 16 workers | 128 workers |
|---|---|---|
| 8192 | 982 ms | 1084 ms |
| 16384 | 1843 ms | 2009 ms |
| 30720 | 3343 ms | 3660 ms |

An 8x increase made it 9~10% slower. This means the fixed cost is a **serial section that concurrency
cannot hide**. It points in the same direction as the observation in the P4 comment of `connector.py`
(per-EQ `eqx_lock` serializes submit+completion, capping at 7~12 GB/s regardless of queue placement).

For this experiment we exposed the hardcoded `self._workers = 16` as the `DAOS_WORKERS` environment
variable. The default stays 16, so existing behavior does not change.

## The blocking path is faster than the non-blocking path

`base/*` uses `batched_get` (blocking). `lw/*` uses `batched_get_non_blocking`.
When the same model predicts base, the measured values are consistently 21~27% faster.

| arm | object size | predicted | measured | |
|---|---|---|---|---|
| base/256 | 40 MiB | 3.31 ms | 2.42 ms | 27% faster |
| base/1024 | 160 MiB | 11.4 ms | 8.97 ms | 21% faster |

This looks like the cost of the asyncio wrapping in the non-blocking path. It again supports the
earlier decision (2026-08-25) to leave `support_batched_get_non_blocking()` as False.

## Upstream bug: double free in the layerwise release path

While `lw/*` runs, the following warning floods out for every memory object.

```
Ref count of MemoryObj <addr> is negative: -1.
Double free occurred somewhere. Setting ref count back to 0 as a hack but please ...
```

The cleanup section of `retrieve_layer` in LMCache 0.5.2 calls `ref_count_down()` twice.
The same structure remains in `dev` (it runs `ref_count_down()` in a loop, then runs another `unpin()`
loop). This should be reported upstream. The feature works, so it does not seem to affect the
performance numbers, but this was not verified.

## Verdict

- **Do not turn on layerwise for read-heavy KV reuse.** At no chunk size does it beat the default
  mode. Even best against best, it is 464 ms vs 269 ms, 1.7x behind.
- The current design (all layers in one object) is **the right choice** on DAOS. It amortizes the
  fixed cost. Layerwise undoes exactly that benefit.
- **It is worth reconsidering for workloads where cold-miss latency matters.** 2x is not small.
  Even then, `chunk_size` must be raised to 1024 or more.

## Translating into a hardware requirement

For `lw/256` to catch up with `base/1024` (269 ms), it must process 4800 objects in 269 ms, so it
needs 0.056 ms per object. It is 0.696 ms now, so it must drop by **about 12x**.

So the quantitative conclusion of this measurement is: "for layer streaming to win, the fixed cost of
an object lookup must become 12x cheaper". This number can serve as the basis for the KV index item on
the DPU side.

## Reproduction

```bash
# client-5 (via cell1)
CHUNK=1024 DAOS_WORKERS=16 bash /root/launch.sh layerwise
podman exec -e ARM=lw_c1024 -e CTXS=8192,16384,30720 vllm-daos \
    python3 /cfg/bench/sweep2.py
```

`launch.sh` is derived from `deploy/launchers/run_vllm_perf_c5.sh` and takes `MODE` (base|layerwise),
`CHUNK`, and `DAOS_WORKERS`.

---

# Follow-up: a dkey/akey layout removes this fixed cost (2026-09-11)

The conclusion above reads like "layerwise does not fit DAOS". More precisely, it was
**layerwise does not fit DFS**. Measuring the same workload again with the raw object API,
the fixed cost collapses.

`tests/obj_latency.c` is that measurement. The layout is exactly the one that
[`doc/lmcache-mp-l2-assessment.md`](https://github.com/gluesys/lmcache-daos/blob/main/doc/lmcache-mp-l2-assessment.md) (Korean) said "is needed to fold" —
one object (class S16, dkeys spread over 16 targets), **dkey = chunk, akey = layer**.

## Results (120 chunks × 40 layers × 1 MiB = 4.69 GiB, 16 threads, S16, rd_fac:0)

| arm | meaning | RPC | wall clock | per object | GB/s |
|---|---|---|---|---|---|
| `sep` | 1 fetch per layer — same shape as layerwise | 4800 | **245 ms** | **0.051 ms** | 20.5 |
| `batch` | 1 fetch per chunk, 40 akeys as an iod array | 120 | **204 ms** | 0.042 ms | 24.7 |
| `one` | single 40 MiB akey per chunk — current storage form | 120 | 571 ms | 0.119 ms | 8.8 |

For comparison: the same shape on the DFS path (4800 × 1 MiB) had a TTFT of 3343 ms.

## Fixed term

We measured `sep` at three sizes and fitted a line.

| layer size | per object |
|---|---|
| 256 KiB | 0.025 ms |
| 1 MiB | 0.050 ms |
| 4 MiB | 0.168 ms |

```
object API : t ≈ 0.0137 ms + 0.0385 ms/MiB     (26 GB/s limit)
DFS        : t ≈ 0.63   ms + 0.067  ms/MiB     (15.7 GB/s limit)
```

The fixed term goes **0.63 → 0.0137 ms**. The slope is also 1.7x better.

## The requirement is exceeded

Above we wrote: "for layerwise to catch up with the best baseline (269 ms) it needs 0.056 ms per
object; it is 0.696 ms now, so 12x". `sep` is **0.051 ms**.

In absolute terms too it is 245 ms vs 269 ms. So moving to dkey/akey makes layerwise faster than the
current best non-layerwise DFS path. With `batch` as well, it is 204 ms.

**This much is left on the table in software alone, before any DPU discussion.**

## Unexpected result: the current storage form is the worst

The `one` arm is the slowest (571 ms, 8.8 GB/s). Against `batch`, the dkeys and the bytes are the same.
The only difference is whether 40 MiB sits as **a single extent** or as **40 akeys of 1 MiB**, yet the
gap is 2.8x. The server seems to process akeys in parallel and a single extent in one line.

This means that, independent of layerwise, the current form (all layers stored contiguously in one
object per chunk) may be **a loss by itself**. It is worth checking separately.

## Incomplete: DFS control group

The DFS number above (0.63 ms) was back-calculated from TTFT, so it may include LMCache engine
overhead. To compare the storage layers alone, we added a DFS arm (`-m dfs`, one file each in a flat
namespace) in the same process, but it **did not run to completion.**

- With 4 files (t=1, L=2, C=2) it works and gives **0.873 ms per object**.
- With more files it fails with `fi_mr_regattr(... iov_len=1048576 ...) failed, rc: -14
  (Bad address), mr_reg_count=4`. It is not registration exhaustion (there are only 4).
  The failing address (`0x7efe1effbeb0`) is **not page-aligned, so it is not our buffer** —
  we allocate with `aligned_alloc(4096, ...)` and use only offsets that are multiples of 1 MiB. It looks
  like a dc_array internal buffer. It does not depend on thread count; it reacts only to file count.
- On the first failure the worker calls `exit(1)`, and the process hangs during shutdown.
  This is a harness defect.

So, separating what is confirmed from what is not:

- **Confirmed**: the fixed cost of the object API is 0.0137 ms, which beats the required 0.056 ms.
  This value does not change however DFS is measured.
- **Unconfirmed**: how much of DFS's 0.63 ms belongs to the storage layer. The single-thread direct
  measurement of 0.873 ms suggests most of it is the storage layer, but it is a 4-file sample.

This is why we do not write the ratio as "46x" and only write **"the requirement is exceeded"**.

## Next

1. Narrow down the registration failure of the DFS arm. If it is a defect in DFS itself, it is worth reporting on its own.
2. Confirm the 2.8x gap of `one` vs `batch`. It may justify changing the current form.
3. Once the above is settled, prototype a dkey/akey backend for `DaosConnector`.
   `daos_obj_fetch_gpu`/`daos_obj_update_gpu` exist in the `theodore/b_cufile` client, so the
   GPU-direct path does not have to be given up (symbols checked).

---

## v3 akey folding measurement (2026-09-30, daos-ib 4 rank / verbs)

Because the measurement above pointed at "fixed cost per object", we wired the connector to fold
layers into akeys under one dkey and send **N iods in one RPC**. Then, on a raw container (PYTHON
layout), we measured folded vs individual calls directly. 40 layers, median of 5 runs, same object
handle reused.

| layer size | write folded | write individual | speedup | read folded | read individual | speedup |
|---|---|---|---|---|---|---|
| 4 KiB | 2.85 ms | 7.89 ms | **2.77x** | 2.26 ms | 7.53 ms | **3.33x** |
| 64 KiB | 7.82 ms | 18.20 ms | **2.33x** | 7.08 ms | 15.11 ms | **2.13x** |
| 256 KiB | 53.89 ms | 54.09 ms | 1.00x | 25.21 ms | 31.33 ms | 1.24x |
| 1 MiB | 241.35 ms | 212.87 ms | 0.88x | 240.76 ms | 224.98 ms | 0.93x |
| 4 MiB | 942.72 ms | 862.12 ms | 0.91x | 933.20 ms | 883.27 ms | 0.95x |

**The crossover is around 256 KiB.** Below it, RPC round trips dominate and the gain is 2~3x. Above
it, bandwidth dominates and folding is actually 5~12% slower.

To be clear about what this means: one layer's size is
`chunk_size(tokens) x KV heads x head_dim x 2(K,V) x 2B`. Qwen3-14B is 4096 B per token per layer,
so with `chunk_size: 256` a layer is 1 MiB — **at the chunk size actually used, payload folding is
not a gain.**

The gain remains in two places.

- **Metadata**: 40 headers finish in one RPC, 1.3 ms for write / 1.9 ms for read. Sent individually,
  that is 40 small RPCs (≈7.5 ms). The read path must read headers first, so this gain always applies.
- **Small-chunk configurations**: with `chunk_size` lowered to 16~64 tokens, 2x or more.

### Limits of this measurement

The individual-call arm ran **sequentially in one thread**. The real connector fans out over a
16-thread pool, so the unfolded path could be faster in practice. So the speedups in the table above
are a **comparison tilted in favor of folding**, and the conclusion that folding loses at 1 MiB and
above is that much firmer. Conversely, the 2~3x at 4~64 KiB should be read as an upper bound.

### Why it crosses there — already explained

This shape is not a new finding. It reproduces, on a different axis, the conclusion of
[`doc/RAW-API-PLAN.md`](https://github.com/gluesys/lmcache-daos/blob/main/doc/RAW-API-PLAN.md) (Korean). That document measured **DFS vs low-level**, and this one
measured **folded vs individual**, and the answer is the same.

- RAW-API-PLAN "Explained — it is all `chunk_size`": DFS is fast because the array API **spreads**
  a byte range **over many dkeys** in `chunk_size` units (default 1 MiB) and issues them concurrently
  within one call. Putting 16 MiB whole into one dkey gives 2.92 GB/s; spreading it in 1 MiB
  chunks gives 17.80 GB/s — our dkey/akey placement corresponds to the former.
- So that document's conclusion was **"what you gain by going low-level is not bandwidth but the
  per-object fixed cost"**, and where low-level actually won was 1.81x on 1 MiB objects
  (metadata and lookup cost).

The table above is exactly that. On small layers, fixed cost dominates, so folding gains 2~3x.
Above 1 MiB, bandwidth dominates, so there is nothing to gain.

**And folding cannot raise this ceiling.** Even when 40 layers are folded into one RPC, those 40
still land on **the same dkey = the same shard**. To recover bandwidth you must spread dkeys like the
array API does, and that is the opposite direction from folding. So folding and striping are
**mutually exclusive optimizations**, and layer size decides which to pick — fold when small,
spread when large.

### Also confirmed along the way

- Consistency: 40/40 metadata and payload, contents match
- `punch_akeys` (newly added): deleting only layer 7 leaves the other 39. Punching the dkey to evict
  one layer would also wipe out the other 39, so this call was needed.
