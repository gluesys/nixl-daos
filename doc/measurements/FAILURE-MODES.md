<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# Failure modes — what LMCache actually sees when DAOS goes wrong

*Translated from [gluesys/lmcache-daos `doc/FAILURE-MODES.md`](https://github.com/gluesys/lmcache-daos/blob/main/doc/FAILURE-MODES.md) (Korean original). File paths refer to the lmcache-daos repository unless noted; the NIXL plugin that lived under `nixl/` there is now `src/plugins/daos/` in this repository.*

Until now, every number in this repo came from a **healthy pool**. That says
nothing about the case that decides whether this backend can ship. A serving
process cannot stop because storage has gone strange, so only one question
really matters: **does a failure arrive as a clean error, as a hang, or as
wrong bytes?**

The verdicts, in this order:

| | | |
|---|---|---|
| **WRONG** | `get()` returns data that `put()` did not write | Not acceptable at any frequency. vLLM serves it as is. The v1 object format has no payload checksum, so nobody except the harness below would notice |
| **HANG** | Does not return within budget | Almost as bad. The request thread is lost, and the blocking DAOS call sits inside a `run_in_executor` thread, so asyncio cancellation does not release it |
| **raise** | Clean failure | Fine, as long as LMCache treats it as a miss |

`miss` is **the correct answer for an object that was not fully written**. The
truncated shape itself is covered directly by `tests/test_torn_object.py`; here
it is checked as the result of a real crash, not of an artificially cut file.

- Harness: `tests/failure_modes.py` (1 scenario = 1 process)
- Driver: `tests/failure_modes.sh` (fault injection + ordering + recovery)
- Unit test: `tests/test_wedge.py` (wedge detection, below)

## Test environment

cxl2 (`memfs.rnd.gluesys.com`), DAOS 2.8.0, 1 engine / 4 targets, tcp
(`enp1s0f0np0`), SCM = 8 GB ramdisk, data = NVMe `0000:c1:00.0` (1.6 TB).

**The ordering is forced, not a preference.** SCM is a ramdisk, so stopping
`daos_server` destroys the pool. All faults the pool survives must run first,
and the server-stop step must come last. This constraint also limits what we
can claim — see "What we could not answer" at the bottom.

## Results

| Fault | Observed | Verdict |
|---|---|---|
| Healthy round trip (6 × 4 MiB) | 6/6 bytes match | ok |
| `daos_agent` SIGKILL, handles open | Reads 6/6 correct; write succeeds (0.00 s) and later reads back correctly | ok |
| After agent returns | 6/6 correct; the pre-failure connector keeps working (0.01 s) | ok |
| Container deleted under open handles | Reads 6/6 raise `DER_NO_HDL(-1002)` immediately; write raises `rc=22` immediately | ok |
| Connector created with no server | `daos_init` returns `DER_TIMEDOUT(-1011)` after **15.2 s** | slow but clean |
| Pool exhaustion (16 GB pool) | `ENOSPC(28)` at 1761 objects (13.8 GiB), **0.01 s**. Reads 8/8 correct; a second write fails the same cheap way | ok |
| **Server SIGKILL during writes** | **16/16 threads still inside `dfs_sys_write` after 16 minutes** | **HANG** |

**No scenario produced wrong bytes.** This is not just a pass. This repo spent
weeks chasing KV corruption, so the result is worth confirming explicitly.

Pool exhaustion is the only failure here that **comes on its own**. Nothing
broke and nobody made a mistake; the cache did its job and ran out of room.
LMCache does not know the pool size, so it keeps calling `put()` forever. What
matters is not that writes fail — they must — but that the failure is
**cheap, repeatable, and does not drag the read path down with it**. A backend
that stops serving hits the moment it stops accepting writes turns a full
cache into an outage. The measurement shows this one does not.

### One confirmed fact: `daos_agent` is not on the I/O path

Once the handles were open, killing the agent left reads and writes working.
The client gets system info and credentials from the agent over a unix socket
at startup, and after that talks to the engines directly over the fabric. This
is measured, not guessed.

One operational trap found on the side: **a SIGKILLed agent does not simply
come back up.**

```
ERROR: daos_agent: unable to start dRPC server: unable to listen on unix socket
/var/run/daos_agent/daos_agent.sock: bind: no such file or directory
```

The cause is that the `/var/run/daos_agent` directory is gone. The systemd unit
declares `RuntimeDirectory=daos_agent`, so systemd owns that directory. Start
it with `systemctl start daos_agent`, or, if you start it by hand, run
`mkdir -p` first.

## The real finding — DAOS calls aimed at a dead engine do not return

SIGKILL of `daos_server` during `batched_put`:

```
Thread 3400968 (idle): "MainThread"
    _wait_for_tstate_lock (threading.py:1169)
    join (threading.py:1149)
    shutdown (concurrent/futures/thread.py:239)
    close (lmcache_daos/connector.py:658)      <- stuck here forever
Thread 3401076 (active): "daos-io_0"
    write_obj_from (lmcache_daos/dfs_binding.py:404)
    _put_sync (lmcache_daos/connector.py:575)
```

| | |
|---|---|
| Stuck threads | **16 / 16** — the whole pool (`DAOS_WORKERS` default) |
| Elapsed | Still in that state at **1016 s** |
| CPU | **1312 %** — about 13 cores. They are not sleeping; they are **spinning** |
| `CRT_TIMEOUT=10` | **No effect.** 13 threads at 786 s, 1411 % |

Three things are true at once.

1. **The pool is gone for the life of the process.** A thread inside a C call
   cannot be cancelled from Python. `asyncio.wait_for()` only gives up waiting;
   the thread is still lost. Every task submitted afterwards queues behind it
   forever.
2. **`close()` did not return.** `self._pool.shutdown(wait=True)` joins those
   threads. A comment just two lines below describes exactly this risk for the
   ping pool and chooses `wait=False`, but the data pool was left at
   `wait=True`.
3. **It burns 13 cores in the serving process.** That is pure throughput loss
   stacked on top of the backend failure.

### All four paths have the same structure

Confirmed by reading the code, not by inference.

| Path | Blocking call | Runs in |
|---|---|---|
| in-process connector | `dfs_sys_*` | `ThreadPoolExecutor` |
| MP L2 adapter | `dfs_sys_*` (`DfsSys` directly) | `ThreadPoolExecutor` |
| NIXL plugin | `daos_obj_fetch/update` | `nixlDaosThreadPool` |
| GDS | same DFS path | same |

We assumed MP mode would be structurally exempt because it is a separate
process. **It is not.** `lmcache_daos/mp/l2_adapter.py` is a thread pool that
uses `DfsSys` directly. Only the in-process connector has been fixed so far.

## What we changed

### 1. `close()` does not join stuck threads

`shutdown(wait=False, cancel_futures=True)`. Only tasks that have not started
are cancelled; a thread already inside libdaos is not something we can
reclaim. `cancel_futures` is 3.9+, so we branch on the version to match the
`>=3.8` floor in `pyproject.toml`.

**This does not make the process exitable.** `concurrent.futures` registers
`_python_exit`, which joins every worker at interpreter exit. Checked directly
— a process holding a single 30 s sleep **lived the full 30 s** after
`shutdown(wait=False)` returned. (That thread was `daemon=True`. Daemon status
does not matter.) The only way out is for the call itself to be bounded, and
that is a DAOS problem, not a Python problem.

### 2. On a detected wedge, fail immediately instead of queueing

Queueing forever is **strictly worse** than failing. LMCache treats a raised
`get`/`put` as a miss and serves from the model, but a submission that never
returns takes the whole request with it.

```python
STALL_SECS = float(os.environ.get("DAOS_STALL_SECS", "60"))

class DaosPoolWedged(DaosError):   # rc = EBUSY
```

The verdict is **"all workers busy + no completion for `STALL_SECS`"**.
Saturation alone is not evidence — a large store legitimately fills 16 workers
for minutes. Under load, completions keep arriving; in a wedge there are
**exactly 0**. Without this distinction, the detector would take the backend
down under the very load it is meant to protect. `tests/test_wedge.py` tests
this false-positive case directly.

`ping()` checks it too. The probe runs on its own thread, so it can cheerfully
return 0 on a pool whose data workers have all been blocked for minutes. The
health check is what decides whether LMCache keeps sending work, so that one
report must not be wrong.

### Effect — the same matrix, run again

What changed before and after the fix:

| | Before | After |
|---|---|---|
| 48 reads after SIGKILL | Each one hangs, only burning budget | **raise=48, hang=0** — immediate failure |
| `close()` | Does not return | Returns immediately |
| Whole driver | Hung at step 4/5, **never reached recovery**, had to be killed by hand | **5/5 + recovery, ran to completion** |

`batched_put` itself still hangs until its budget (30 s). That is an unbounded
DAOS call and cannot be touched from Python. What changed is **everything that
comes after it**.

**Cost**: 0.59 µs per operation (2 locks + 1 wrapper call). That is **0.78 %**
of the measured NIXL per-request overhead of 76 µs, and negligible next to
630 µs per DFS object. Measured with `timeit` over 200,000 runs, not estimated.

## Is the low-level object API different — measured

The reason to move from DFS to dkey/akey is performance. The per-object fixed
cost is **0.63 ms vs 0.0137 ms**, a 46x difference
([`LAYERWISE-MEASUREMENT.md`](LAYERWISE-MEASUREMENT.md)). But nobody had
measured how the low-level API behaves under the same fault, and "it is a
different API" is not evidence. `tests/obj_failure.c` measures it. Exactly the
same fault as above — `daos_server` SIGKILL during writes, 16 threads, 8 MiB
updates.

| arm | Result | Threads lost |
|---|---|---|
| `sync` — `daos_obj_update(..., NULL)` | **16/16 still inside the call after 150 s** | **16** |
| `async` — event queue + `daos_eq_poll` timeout | 16/16 give up on their own at **5.01 s**, 5.4 s total | **0** |
| `async-abort` — above + `daos_event_abort`/`fini` | Same. **abort 0.00 s, fini 0.00 s** | **0** |

**The difference is not "low-level vs DFS" but "blocking vs event queue".** The
blocking form of the low-level API wedges exactly like DFS — 16/16,
indistinguishable. What DFS cannot do at all is the event-queue side.

And **the NIXL plugin currently uses the blocking form.**
`nixl/plugin/daos_backend.cpp` calls `daos_obj_update(..., nullptr)`. So
moving to the low-level API alone does not improve failure behavior at all. It
has to go all the way to the event queue.

### Why this is independent of the open question above

Earlier we left open the possibility that the infinite wedge is a single-rank
artifact — SWIM has no quorum to evict the dead rank. **The `async` result is
not affected by that question.** The 5.01 s bound was **set by the caller**,
not produced by DAOS giving up. The result is the same whether DAOS retries
forever or gives up at 60 s. The event queue simply has the property that the
connector currently has to imitate with the wedge detector.

### There is a price

When `daos_eq_poll` returns on timeout, **the RPC is still in flight.** The
thread is reclaimed, but the operation is not cancelled. If the event is just
dropped, DAOS ends up pointing at a stack frame that is about to die — the
first `async` arm we measured did exactly that, which is why we built a
separate `async-abort` arm. The result is **0.00 s for both abort and fini**,
so cleanup costs nothing. The escape hatch does not move the blockage
elsewhere.

### The GPU entry points take events too (the comment was wrong)

The plugin had a comment saying "GPU entry points are sync-only — no event
queue", and used it to justify "so we run them on threads". **That was
wrong.**

In `src/client/api/object.c` on `theodore/b_cufile`, the two functions handle
`ev` **identically**. The only differences are the GPU_DIRECT flag and
`mem_attrs`.

```c
:197 daos_obj_update     → dc_obj_update_task_create(oh, th, flags,              ..., ev, NULL, &task)
:213 daos_obj_update_gpu → dc_obj_update_task_create(oh, th, flags|GPU_DIRECT,   ..., ev, NULL, &task)
                           args->mem_attrs = mem_attrs
```

The declarations agree — `daos_obj_update_gpu(..., daos_mem_attr_t *mem_attrs,
daos_event_t *ev)`, `daos_obj_fetch_gpu(..., daos_mem_attr_t *mem_attrs,
daos_iom_t *ioms, daos_event_t *ev)`. The header comment also points to the
non-GPU version and states that a NULL `ev` means blocking.

So the plugin's `nullptr` is **a choice, not a constraint**, and right now it
is the wrong choice. The `VRAM_SEG` path can also get the 5 s escape hatch of
the `async` arm above.

**Scope of verification**: we read the **source** of the draft branch. If the
binary installed on the GPU host was built from this branch, it is the same,
but we did not check that. We also **did not measure** the failure behavior of
the GPU path — we only established that the API accepts events. Measuring it
needs a GPU host.

## Fix three: the NIXL plugin gets a deadline

Set `NIXL_DAOS_EQ_TIMEOUT` to a number of seconds and the plugin submits to an
event queue instead of making a blocking call, and polls with that deadline.
Unset or `0` keeps the old blocking behavior.

Killing `daos_server` during reads through the plugin:

| | Result |
|---|---|
| Blocking | Still alive after **120 s**, 65 threads stuck |
| `NIXL_DAOS_EQ_TIMEOUT=8` | Exits in **8 s**, all threads reclaimed |

### Per-thread EQs were a trap

At first we created one EQ per thread with `thread_local`. It was **35% slower
than blocking** (2.08 vs 3.12 GB/s). The thread pool is deliberately sized
large — 64 threads handle at most `inflight` requests. **An idle thread is
free, but an idle EQ holds a network context.** We were buying 64 contexts to
run 16 requests.

Changing to **borrowing** an EQ per request removed it. An EQ is held only for
the duration of one request, so the set grows only up to the real peak
concurrency and no further. It no longer depends on the thread count.

| | 64 threads | 16 threads |
|---|---|---|
| Blocking | 3.16 GB/s | 3.12 GB/s |
| EQ, 1 per thread | 2.08 | 3.24 |
| EQ, borrowed | **3.24** | **3.26** |

As a bonus, it keeps a property the per-thread scheme had for free — two
threads never hold the same EQ at once, so polling cannot steal someone else's
completion.

### Re-measured on 400G verbs — no cost

The sweep above was run on cxl2 (single-node TCP, 3 GB/s), so it **did not
reach the range where blocking had won.** We re-measured 4 times each with the
same harness on client-5 ↔ cell1/cell2, `ofi+verbs;ofi_rxm`, 2 ranks × 8
targets.

| | Runs | Mean |
|---|---|---|
| Blocking | 31.29 · 31.00 · 30.70 · 31.15 | **31.04 GB/s** |
| Event queue | 30.44 · 31.27 · 30.23 · 30.61 | **30.64 GB/s** |

**The ranges overlap.** In some runs the event queue beat blocking. The
unfolded case (4800 requests) is also equal, 7.55 vs 7.50.

The old rejection rationale ("EQ gives 7–12 GB/s however it is arranged")
**does not carry over to this form.** So **we turned it on by default.**
Turning 16 permanently lost threads into a bounded error with a deadline that
costs nothing is not a trade-off.

The 60 s default is not a latency target but a safety net against a dead
engine — one request on verbs took 1.34 ms, so there are four orders of
magnitude of headroom.

### A trap hit while measuring: container `rd_fac`

At first the measurement did not work at all. `test_xfer` ran for 20 minutes at
99.8 % CPU, repeating `DER_HG(-1020)` on rank 0 tag 7 every 15 seconds. Seeing
that the plugin has no warm-up code, we suspected the known rdma_cm stall.
**It was not that.**

A container created with pool defaults had `rd_fac=1`, and every working
container had `rd_fac=0`. Creating it with `--properties=rd_fac:0` passed
11/11. The original plugin dated 9/12 hung the same way, so **it is not a
plugin defect.**

On the same pool, DFS (`daos fs check`) completes normally in 1 second. Only
the low-level object path hangs.

### Old default decision (record)

The old rejection rationale in
[`doc/DESIGN-AND-VALIDATION.md`](https://github.com/gluesys/lmcache-daos/blob/main/doc/DESIGN-AND-VALIDATION.md)
(Korean) ("EQ gives 7–12 GB/s however it is arranged") came from a sweep that
**read 28 MiB from Python over the DFS async path**. It does not reproduce on
folded object-API requests. Nor is it refuted — cxl2 is single-node TCP, so the
fabric is the bottleneck at 3 GB/s, and it **does not reach the 400G verbs
34 GB/s range where blocking had won.** That is where the per-EQ network
context cost shows up. Changing the default needs that host.

## What we could not answer

These are limits, not TODOs. They cannot be answered on the current hardware.

| Question | Why not | What it needs |
|---|---|---|
| Does an object in flight during the crash stay on disk **truncated** (→ miss, correct) or **complete but wrong** | SCM is a ramdisk, so recovery = reformat. The evidence disappears with it | MD-on-SSD |
| Is the infinite retry a **single-rank property** | There is only one rank, so SWIM has no quorum to evict the dead rank. A multi-rank cluster might have raised an error after eviction (but the `async` result above does not depend on this question) | cell1/cell2 |
| Throughput of the EQ path on 400G verbs | cxl2 is fabric-bound at 3 GB/s, so it cannot be told apart from blocking | cell1/client-5 |
| **Measured** failure behavior of the GPU path | Confirmed in source that it accepts events. Whether it actually exits at 5 s has not been measured | GPU host |
| Rank loss · network partition | There is one node | cell1/cell2 |


The first two rows matter. The infinite wedge **may be** a single-node
artifact, and if so the severity drops. But **the client-side structure is the
same regardless of cluster size** — the connector has no deadline of its own on
the data path. The ping path got `PING_TIMEOUT_SECS`, but reads and writes did
not. What we added this time is not a deadline but **recognizing a wedge and
no longer queueing behind it**.

## How to run

```bash
sudo bash tests/failure_modes.sh [pool] [container]     # full matrix (destructive!)
python3 tests/test_wedge.py                             # wedge detection (no DAOS needed)

# low-level path (destructive!) -- arm: sync | async | async-abort
gcc -O2 -pthread -o obj_failure tests/obj_failure.c \
    -I/usr/include -L/usr/lib64 -ldaos -ldaos_common -lgurt -luuid
./obj_failure --pool kvpool --cont nixltest --arm async --kill-after 0.3
```

The driver **breaks and recovers** a single-node DAOS. Never run it on a shared
cluster.
