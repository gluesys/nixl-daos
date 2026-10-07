<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Gluesys Co., Ltd. -->

# Security policy

## Reporting

Email **kpkim@gluesys.com** with `[nixl-daos]` in the subject. Please do not open
a public issue for a suspected vulnerability.

Include what an attacker can do, the NIXL and DAOS versions, the transport
(`ofi+verbs;ofi_rxm`, `ofi+tcp`, ...), and the smallest reproduction you have. If
you have a patch, attach it rather than opening a pull request, since pull
requests are public.

We will acknowledge within five working days. This is a small team with no
dedicated on-call, so read that as an honest expectation rather than a service
level. We will tell you what we intend to do and when, and credit you in the fix
unless you ask us not to.

## What is in scope

This is a storage backend for NIXL, used for LLM inference KV caches. The
interesting failure is usually not a crash.

- **Returning the wrong bytes**: a read that completes with data from another
  object, another offset or an older write. This is treated as a security issue
  even when it looks like a plain bug, because it is silent: lengths and status
  codes stay correct and the model produces fluent, wrong text.
- **Crossing a container or object boundary**: a descriptor that reaches an
  object or container it did not name.
- **Memory safety** in the plugin: out-of-bounds access through descriptor
  addresses or lengths, use-after-free around request release or engine teardown.
- **Hangs a client can trigger cheaply**, for example by leaving a request
  waiting on an engine that will never answer.

## What is out of scope

- **DAOS, NIXL, UCX, Mercury and libfabric themselves.** Report those to their
  projects. If the issue is in how *this* code uses them, it is in scope.
- **`VRAM_SEG` (GPU-direct).** It depends on a DAOS client with GPU support that
  is not in a released DAOS, and is experimental. We will read reports and fix
  what we can, but it carries no security promise.
- **Misconfiguration of the storage underneath.** Two DAOS ranks formatting the
  same dual-port drives corrupt data, and no care in this code prevents it.
- **Access control on DAOS pools and containers.** The plugin opens what the
  process is allowed to open; isolation between tenants is the deployer's job
  (pool and container ACLs, separate `daos_agent` credentials).

## Supported versions

There is no tagged release. Only `main` is supported, and fixes land there.

| Version | Supported |
|---|---|
| `main` | yes |
| anything else | no |

## Hardening notes for deployers

- Verify that no two DAOS ranks own the same physical drive before trusting any
  data: compare PCI DSNs across nodes and confirm each rank reports distinct
  device UUIDs.
- Objects carry no payload checksum from this plugin. Integrity on the wire is
  the transport's; if you need more, use a DAOS object class with redundancy or
  enable DAOS container checksums, and verify at the application layer.
- Set `NIXL_DAOS_EQ_TIMEOUT` (default 60 s). `0` disables the deadline, and a
  request against a dead engine then never returns.
