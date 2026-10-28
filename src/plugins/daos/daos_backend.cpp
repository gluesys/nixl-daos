/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 Gluesys Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "daos_backend.h"

#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "common/configuration.h"
#include "common/nixl_log.h"

namespace {

/*
 * metaInfo grammar, kept deliberately small:
 *
 *   "pool/container"                 open (or reuse) the container, then
 *                                    generate an object id from devId
 *   "pool/container/<hi>.<lo>"       use that exact object id
 *
 * The two-field form is what a cache uses: the caller already has a unique
 * devId per object, so deriving the oid from it keeps the caller from having
 * to invent and remember object ids. The three-field form exists so an
 * existing object can be addressed, which is what any test that writes with
 * one tool and reads with another needs.
 */
struct daosTarget {
    std::string pool;
    std::string cont;
    bool haveOid = false;
    daos_obj_id_t oid{};
};

bool
parseTarget(const std::string &meta, daosTarget &out) {
    std::vector<std::string> parts;
    std::stringstream ss(meta);
    std::string tok;

    while (std::getline(ss, tok, '/')) {
        if (!tok.empty()) {
            parts.push_back(tok);
        }
    }

    if (parts.size() < 2 || parts.size() > 3) {
        return false;
    }

    out.pool = parts[0];
    out.cont = parts[1];

    if (parts.size() == 3) {
        const auto dot = parts[2].find('.');
        if (dot == std::string::npos) {
            return false;
        }
        try {
            out.oid.hi = std::stoull(parts[2].substr(0, dot));
            out.oid.lo = std::stoull(parts[2].substr(dot + 1));
        }
        catch (const std::exception &) {
            return false;
        }
        out.haveOid = true;
    }
    return true;
}

/*
 * A numeric setting from the NIXL configuration (environment variable or config
 * file). A value that does not parse is reported and replaced by the default
 * rather than failing the backend.
 */
template<typename T>
T
configValue(const char *key, T fallback) {
    try {
        return nixl::config::getValueDefaulted<T>(key, fallback);
    }
    catch (const std::exception &e) {
        NIXL_WARN << key << " ignored (" << e.what() << ")";
        return fallback;
    }
}

} // namespace

nixlDaosThreadPool::nixlDaosThreadPool(unsigned n) {
    workers_.reserve(n);
    for (unsigned i = 0; i < n; i++) {
        workers_.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> g(m_);
                    cv_.wait(g, [this] { return stop_ || !q_.empty(); });
                    if (stop_ && q_.empty()) {
                        return;
                    }
                    job = std::move(q_.front());
                    q_.pop();
                }
                job();
            }
        });
    }
}

nixlDaosThreadPool::~nixlDaosThreadPool() {
    {
        std::lock_guard<std::mutex> g(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void
nixlDaosThreadPool::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> g(m_);
        q_.push(std::move(job));
    }
    cv_.notify_one();
}

nixl_mem_list_t
nixlDaosEngine::supportedMems() {
#ifdef NIXL_DAOS_HAVE_GPU
    return {FILE_SEG, DRAM_SEG, VRAM_SEG};
#else
    return {FILE_SEG, DRAM_SEG};
#endif
}

nixl_b_params_t
nixlDaosEngine::getPluginParams() {
    /* What createBackend() accepts. Both also have an environment form through
     * nixl::config, which stays for deployments that set it once for a whole
     * process; a value given here wins, because it is the only way to give two
     * DAOS backends in one agent different settings.
     *
     * "pool" and "container" will join these so a deployment can set a default
     * instead of repeating it in every metaInfo. */
    return nixl_b_params_t{
        {"threads", "IO threads, 1..512 (default 64, env NIXL_DAOS_THREADS)"},
        {"eq_timeout",
         "seconds a request may wait in the event queue before it is abandoned; "
         "0 makes every call blocking (default 60, env NIXL_DAOS_EQ_TIMEOUT)"},
    };
}

/* DAOS error -> NIXL status. Without this everything that is not a bad
 * argument collapses into NIXL_ERR_BACKEND, and a caller cannot tell "the
 * object is not there" from "the engine died" -- which is exactly the
 * distinction a cache needs, and the one the event-queue deadline exists to
 * surface. DAOS returns negated DER_* codes. */
nixl_status_t
daosToNixl(int rc) {
    switch (rc) {
    case 0:
        return NIXL_SUCCESS;
    case -DER_NONEXIST:
        return NIXL_ERR_NOT_FOUND;
    case -DER_NO_PERM:
        return NIXL_ERR_NOT_ALLOWED;
    case -DER_INVAL:
    case -DER_NO_HDL:
        return NIXL_ERR_INVALID_PARAM;
    case -DER_TIMEDOUT:
    case -DER_UNREACH:
    case -DER_EXCLUDED:
        /* The pool is unreachable or the rank serving it is gone. The caller's
         * recovery for this is not the same as for a malformed request. */
        return NIXL_ERR_REMOTE_DISCONNECT;
    case -DER_CANCELED:
        return NIXL_ERR_CANCELED;
    default:
        return NIXL_ERR_BACKEND;
    }
}

/* Read a backend parameter, falling back to the environment/config form.
 * getInitParam() answers NIXL_ERR_INVALID_PARAM when the key is simply absent,
 * which is not an error here. */
long
nixlDaosEngine::param(const char *key, const char *envKey, long fallback) const {
    std::string v;
    if (getInitParam(key, v) == NIXL_SUCCESS) {
        try {
            return std::stol(v);
        }
        catch (const std::exception &) {
            NIXL_WARN << "backend parameter " << key << "=\"" << v
                      << "\" is not a number; falling back to " << envKey;
        }
    }
    return configValue<long>(envKey, fallback);
}

nixlDaosEngine::nixlDaosEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params) {
    eqPool_ = std::make_unique<nixlDaosEqPool>();
    const int rc = daos_init();

    /* -DER_ALREADY: another component in this process already called it. That
     * is normal when the application also links the DAOS client, so it counts
     * as success for us -- but we must then not call daos_fini() on the way
     * out, or we pull the library out from under them. */
    if (rc == 0) {
        daosInited_ = true;
    } else if (rc != -DER_ALREADY) {
        /* Without the client library nothing below can work; report it so the
         * agent refuses this backend instead of handing out one that fails on
         * every call. The destructor copes with the members left unset. */
        NIXL_ERROR << "daos_init() failed: " << rc;
        initErr = true;
        return;
    }

    /* 64 is where the concurrency ladder flattens on the testbed: at 400G
     * verbs the unfolded arm reaches 8.3 GB/s at 16 threads, 11.3 at 32,
     * 14.4 at 64, and 14.3 at 128. Effective concurrency is min(threads,
     * requests in flight), so a caller that keeps fewer requests outstanding
     * is capped by its own depth rather than by this number. */
    unsigned nthreads = 64;
    const long v = param("threads", "NIXL_DAOS_THREADS", nthreads);
    if (v > 0 && v <= 512) {
        nthreads = static_cast<unsigned>(v);
    } else {
        NIXL_WARN << "threads=" << v << " ignored (expected 1..512)";
    }
    pool_ = std::make_unique<nixlDaosThreadPool>(nthreads);

    /* Whole seconds: NIXL's configuration layer has no floating-point type. */
    const long eqt = param("eq_timeout", "NIXL_DAOS_EQ_TIMEOUT", 60);
    if (eqt >= 0) {
        eqTimeout_ = static_cast<double>(eqt);
    } else {
        NIXL_WARN << "eq_timeout=" << eqt << " ignored (expected >= 0)";
    }
    NIXL_DEBUG << "DAOS backend: " << nthreads << " IO threads, eq_timeout " << eqTimeout_ << "s";
}

nixlDaosEngine::~nixlDaosEngine() {
    /* Workers touch container handles through the object handles they were
     * given, so they have to be gone before anything below is closed. */
    pool_.reset();

    /* Each event queue owns a network context; daos_fini() refuses to run while
     * one is open (DER_BUSY, "cannot finalize, current ctx_num"), and the pool's
     * own destructor would otherwise run after it, destroying queues in a library
     * that has already been shut down. */
    eqPool_.reset();

    {
        std::lock_guard<std::mutex> g(mtx_);
        for (auto &kv : conts_) {
            if (kv.second.refs > 0) {
                NIXL_WARN << "DAOS container " << kv.first << " still had " << kv.second.refs
                          << " registration(s) at shutdown";
            }
            daos_cont_close(kv.second.coh, nullptr);
            daos_pool_disconnect(kv.second.poh, nullptr);
        }
        conts_.clear();
    }
    if (daosInited_) {
        daos_fini();
    }
}

nixl_status_t
nixlDaosEngine::getCont(const std::string &pool, const std::string &cont, contHandles *&out) const {
    const std::string key = pool + "/" + cont;
    auto it = conts_.find(key);

    if (it != conts_.end()) {
        it->second.refs++;
        out = &it->second;
        return NIXL_SUCCESS;
    }

    contHandles h{};
    int rc = daos_pool_connect(pool.c_str(), nullptr, DAOS_PC_RW, &h.poh, nullptr, nullptr);
    if (rc != 0) {
        NIXL_ERROR << "daos_pool_connect(" << pool << ") failed: " << rc;
        return daosToNixl(rc);
    }

    rc = daos_cont_open(h.poh, cont.c_str(), DAOS_COO_RW, &h.coh, nullptr, nullptr);
    if (rc != 0) {
        NIXL_ERROR << "daos_cont_open(" << cont << ") failed: " << rc;
        daos_pool_disconnect(h.poh, nullptr);
        return daosToNixl(rc);
    }

    h.refs = 1;
    out = &conts_.emplace(key, h).first->second;
    return NIXL_SUCCESS;
}

void
nixlDaosEngine::putCont(const std::string &pool, const std::string &cont) const {
    const std::string key = pool + "/" + cont;
    auto it = conts_.find(key);
    if (it == conts_.end()) {
        return;
    }

    if (--it->second.refs > 0) {
        return;
    }

    daos_cont_close(it->second.coh, nullptr);
    daos_pool_disconnect(it->second.poh, nullptr);
    conts_.erase(it);
}

nixl_status_t
nixlDaosEngine::registerMem(const nixlBlobDesc &mem,
                            const nixl_mem_t &nixl_mem,
                            nixlBackendMD *&out) {
    out = nullptr;

    /* Host and device buffers are the transfer source/sink, not something DAOS
     * holds: the buffer is handed to the fetch as an sgl at transfer time and
     * needs no registration of its own. Accept them so the agent can pair one
     * with a FILE descriptor, and return no metadata.
     *
     * Device memory needs no registration here either. CaRT registers the
     * buffer when it builds the bulk handle, unless a pre-registered RDMA key
     * is supplied in daos_mem_attr_t::ma_rkey -- which is the path that avoids
     * double registration when cuFile already owns the buffer, and is left for
     * when there is a cuFile-registered pool to test against. */
    if (nixl_mem == DRAM_SEG) {
        return NIXL_SUCCESS;
    }
#ifdef NIXL_DAOS_HAVE_GPU
    if (nixl_mem == VRAM_SEG) {
        return NIXL_SUCCESS;
    }
#endif

    if (nixl_mem != FILE_SEG) {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    daosTarget t;
    if (!parseTarget(mem.metaInfo, t)) {
        NIXL_ERROR << "DAOS metaInfo must be \"pool/container\" or "
                      "\"pool/container/<hi>.<lo>\", got: "
                   << mem.metaInfo;
        return NIXL_ERR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> g(mtx_);

    contHandles *ch = nullptr;
    const nixl_status_t st = getCont(t.pool, t.cont, ch);
    if (st != NIXL_SUCCESS) {
        return st;
    }

    daos_obj_id_t oid = t.oid;
    if (!t.haveOid) {
        /* Derive the object id from devId. Deterministic on purpose: the same
         * devId must name the same object across process restarts, otherwise a
         * cache cannot find what it stored.
         *
         * DAOS_OT_MULTI_UINT64, not MULTI_HASHED: the dkey and akey this
         * backend writes *are* uint64 (addr / dkeySpan_ and addr %
         * dkeySpan_), and declaring them hashed throws that ordering away.
         * DAOS then refuses an ordered query -- daos_obj_query_key() answers
         * "Can't query non UINT64 typed Dkeys" (-DER_INVAL) -- which is what
         * queryMem() needs to find the end of an object. */
        oid.hi = 0;
        oid.lo = mem.devId;
        const int rc = daos_obj_generate_oid(ch->coh, &oid, DAOS_OT_MULTI_UINT64, OC_UNKNOWN, 0, 0);
        if (rc != 0) {
            NIXL_ERROR << "daos_obj_generate_oid failed: " << rc;
            putCont(t.pool, t.cont);
            return daosToNixl(rc);
        }
    }

    daos_handle_t oh;
    const int rc = daos_obj_open(ch->coh, oid, DAOS_OO_RW, &oh, nullptr);
    if (rc != 0) {
        NIXL_ERROR << "daos_obj_open failed: " << rc;
        putCont(t.pool, t.cont);
        return daosToNixl(rc);
    }

    out = new nixlDaosObjMD(t.pool, t.cont, oh, oid, mem.devId);
    return NIXL_SUCCESS;
}

nixl_status_t
nixlDaosEngine::deregisterMem(nixlBackendMD *meta) {
    /* DRAM registrations produced no metadata; nothing to undo. */
    if (meta == nullptr) {
        return NIXL_SUCCESS;
    }

    auto *md = dynamic_cast<nixlDaosObjMD *>(meta);
    if (md == nullptr) {
        return NIXL_ERR_INVALID_PARAM;
    }

    std::lock_guard<std::mutex> g(mtx_);

    const int rc = daos_obj_close(md->oh, nullptr);
    if (rc != 0) {
        NIXL_ERROR << "daos_obj_close failed: " << rc;
    }

    putCont(md->pool, md->cont);
    delete md;

    return daosToNixl(rc);
}

/* ---- transfer ------------------------------------------------------------ */

nixl_status_t
nixlDaosEngine::queryMem(const nixl_reg_dlist_t &descs,
                         std::vector<nixl_query_resp_t> &resp) const {
    /* std::nullopt means "not here". A descriptor that cannot be parsed, or
     * whose container will not open, is reported the same way rather than
     * failing the whole list: the caller asked about several objects and the
     * answer for one of them should not hide the answer for the rest. OBJ does
     * the same. */
    resp.assign(static_cast<size_t>(descs.descCount()), std::nullopt);

    for (int i = 0; i < descs.descCount(); ++i) {
        const nixlBlobDesc &d = descs[i];

        daosTarget t;
        if (!parseTarget(d.metaInfo, t)) {
            NIXL_WARN << "queryMem: DAOS metaInfo must be \"pool/container\" or "
                         "\"pool/container/<hi>.<lo>\", got: "
                      << d.metaInfo;
            continue;
        }

        std::lock_guard<std::mutex> g(mtx_);

        contHandles *ch = nullptr;
        if (getCont(t.pool, t.cont, ch) != NIXL_SUCCESS) {
            continue;
        }

        daos_obj_id_t oid = t.oid;
        if (!t.haveOid) {
            /* Same derivation as registerMem(): the point of querying is to
             * learn about the object a later transfer would touch. */
            oid.hi = 0;
            oid.lo = d.devId;
            const int rc =
                daos_obj_generate_oid(ch->coh, &oid, DAOS_OT_MULTI_UINT64, OC_UNKNOWN, 0, 0);
            if (rc != 0) {
                NIXL_ERROR << "queryMem: daos_obj_generate_oid failed: " << rc;
                putCont(t.pool, t.cont);
                continue;
            }
        }

        daos_handle_t oh;
        int rc = daos_obj_open(ch->coh, oid, DAOS_OO_RO, &oh, nullptr);
        if (rc != 0) {
            /* Opening an object that was never written succeeds in DAOS, so
             * this is a real failure, not an absent object. */
            NIXL_ERROR << "queryMem: daos_obj_open failed: " << rc;
            putCont(t.pool, t.cont);
            continue;
        }

        /* Largest (dkey, akey, extent) the object holds. DAOS creates objects
         * lazily, so "never written" comes back either as -DER_NONEXIST or as
         * success with an empty extent -- the header documents the second. */
        uint64_t dkeyVal = 0;
        uint64_t akeyVal = 0;
        daos_key_t dkey;
        daos_key_t akey;
        daos_recx_t recx{};
        d_iov_set(&dkey, &dkeyVal, sizeof(dkeyVal));
        d_iov_set(&akey, &akeyVal, sizeof(akeyVal));

        rc = daos_obj_query_key(oh,
                                DAOS_TX_NONE,
                                DAOS_GET_DKEY | DAOS_GET_AKEY | DAOS_GET_RECX | DAOS_GET_MAX,
                                &dkey,
                                &akey,
                                &recx,
                                nullptr);

        const int close_rc = daos_obj_close(oh, nullptr);
        if (close_rc != 0) {
            NIXL_ERROR << "queryMem: daos_obj_close failed: " << close_rc;
        }
        putCont(t.pool, t.cont);

        if (rc == -DER_NONEXIST || (rc == 0 && recx.rx_nr == 0)) {
            continue;
        }
        if (rc != 0) {
            NIXL_ERROR << "queryMem: daos_obj_query_key failed: " << rc;
            continue;
        }

        /* Offsets are flattened the way prepXfer() unflattens them:
         * dkey = addr / dkeySpan_, akey = addr % dkeySpan_. The highest byte
         * the object holds is therefore the end of the largest extent under
         * the largest key pair. */
        const uint64_t end = dkeyVal * dkeySpan_ + akeyVal + recx.rx_idx + recx.rx_nr;
        resp[static_cast<size_t>(i)] = nixl_b_params_t{{"size", std::to_string(end)}};
    }

    return NIXL_SUCCESS;
}

/*
 * Group the paired descriptors by (object, dkey) and build the DAOS structures
 * once. Nothing is submitted here.
 *
 * Pairing follows the convention every storage backend in NIXL uses and POSIX
 * documents by example: local and remote are walked together by index, local
 * is the DRAM buffer and remote is the storage side, so remote->addr is the
 * offset and remote->len the length.
 */
nixl_status_t
nixlDaosEngine::prepXfer(const nixl_xfer_op_t &operation,
                         const nixl_meta_dlist_t &local,
                         const nixl_meta_dlist_t &remote,
                         const std::string &remote_agent,
                         nixlBackendReqH *&handle,
                         const nixl_opt_b_args_t *opt_args) const {
    handle = nullptr;

    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "DAOS: descriptor counts differ (local " << local.descCount() << ", remote "
                   << remote.descCount() << ")";
        return NIXL_ERR_INVALID_PARAM;
    }
    if (local.descCount() == 0) {
        return NIXL_ERR_INVALID_PARAM;
    }

    /* The local side is the memory buffer and the remote side the DAOS object;
     * the other way round, an object offset would be used as a pointer. */
    const nixl_mem_t local_type = local.getType();
    bool local_ok = local_type == DRAM_SEG;
#ifdef NIXL_DAOS_HAVE_GPU
    local_ok = local_ok || local_type == VRAM_SEG;
#endif
    if (!local_ok || remote.getType() != FILE_SEG) {
        NIXL_ERROR << "DAOS: expected a memory local list and a FILE_SEG remote list";
        return NIXL_ERR_INVALID_PARAM;
    }

    /* First pass: bucket by (object handle, dkey) without touching the DAOS
     * structures, so the vectors can be sized exactly before any pointer into
     * them is taken. */
    struct entry {
        uint64_t akey;
        size_t len;
        void *buf;
        uint64_t localDev; /* CUDA ordinal when the local side is VRAM */
    };

    std::map<std::pair<uint64_t, uint64_t>, std::pair<daos_handle_t, std::vector<entry>>> buckets;

    auto li = local.begin();
    auto ri = remote.begin();
    for (; li != local.end() && ri != remote.end(); ++li, ++ri) {
        auto *md = dynamic_cast<nixlDaosObjMD *>(ri->metadataP);
        if (md == nullptr) {
            NIXL_ERROR << "DAOS: remote descriptor has no DAOS metadata; was it "
                          "registered with FILE_SEG?";
            return NIXL_ERR_INVALID_PARAM;
        }
        if (li->len != ri->len) {
            NIXL_ERROR << "DAOS: paired descriptors differ in length (" << li->len << " vs "
                       << ri->len << ")";
            return NIXL_ERR_INVALID_PARAM;
        }

        const uint64_t dkey = ri->addr / dkeySpan_;
        const uint64_t akey = ri->addr % dkeySpan_;
        auto &slot = buckets[{md->devId, dkey}];
        slot.first = md->oh;
        slot.second.push_back({akey, ri->len, reinterpret_cast<void *>(li->addr), li->devId});
    }

    auto req = std::make_unique<nixlDaosBackendReqH>();
    req->op = operation;
    req->groups.reserve(buckets.size());

#ifdef NIXL_DAOS_HAVE_GPU
    /* The descriptor list carries the segment type, so the local side tells us
     * whether these buffers are device memory. devId on a VRAM descriptor is
     * the CUDA ordinal. */
    const bool local_is_gpu = local_type == VRAM_SEG;
#endif

    for (auto &kv : buckets) {
        const auto &ents = kv.second.second;
        nixlDaosIoGroup g;
        g.oh = kv.second.first;
        g.dkeyVal = kv.first.second;

        const size_t n = ents.size();
        g.akeyVals.resize(n);
        g.iods.resize(n);
        g.recxs.resize(n);
        g.sgls.resize(n);
        g.iovs.resize(n);

        for (size_t i = 0; i < n; i++) {
            g.akeyVals[i] = ents[i].akey;

            g.recxs[i].rx_idx = 0;
            g.recxs[i].rx_nr = ents[i].len;

            g.iods[i] = daos_iod_t{};
            d_iov_set(&g.iods[i].iod_name, &g.akeyVals[i], sizeof(uint64_t));
            g.iods[i].iod_type = DAOS_IOD_ARRAY;
            g.iods[i].iod_size = 1; /* byte records */
            g.iods[i].iod_nr = 1;
            g.iods[i].iod_recxs = &g.recxs[i];

            d_iov_set(&g.iovs[i], ents[i].buf, ents[i].len);
            g.sgls[i].sg_nr = 1;
            g.sgls[i].sg_nr_out = 0;
            g.sgls[i].sg_iovs = &g.iovs[i];
        }

#ifdef NIXL_DAOS_HAVE_GPU
        if (local_is_gpu) {
            g.memAttrs.resize(n);
            for (size_t i = 0; i < n; i++) {
                g.memAttrs[i] = daos_mem_attr_t{};
                g.memAttrs[i].ma_mem_type = DAOS_MEM_TYPE_CUDA;
                g.memAttrs[i].ma_device_id = ents[i].localDev;
                /* ma_rkey left zeroed: CaRT registers the buffer itself. */
            }
        }
#endif
        req->groups.push_back(std::move(g));
    }

    NIXL_DEBUG << "DAOS: prepared " << local.descCount() << " descriptor(s) as "
               << req->groups.size() << " RPC(s)";

    handle = req.release();
    return NIXL_SUCCESS;
}

/*
 * Event-queue path: on by default, NIXL_DAOS_EQ_TIMEOUT=0 turns it off.
 *
 * https://github.com/gluesys/nixl-daos/blob/main/doc/measurements/FAILURE-MODES.md
 * measured why it is wanted:
 * when the engine is killed mid-write, a blocking daos_obj_update() never returns -- 16 of 16
 * threads still inside the call at 150 s -- while the same work submitted to an event queue and
 * polled with a timeout released every thread at 5.01 s, and the
 * daos_event_abort()/daos_event_fini() that tidies up cost 0.00 s each.
 *
 * The original rejection
 * (https://github.com/gluesys/lmcache-daos/blob/main/doc/DESIGN-AND-VALIDATION.md) measured 1 EQ
 * capping at ~12.5 GB/s and 16 EQs collapsing to 2.74, against 34-35 GB/s blocking -- but on the
 * DFS async path through Python with 28 MiB reads, which is not this. It does not transfer.
 * Measured on client-5 against cell1/cell2, 400G verbs, the regime that rejection came from, four
 * runs each:
 *
 *   blocking   31.29  31.00  30.70  31.15    mean 31.04 GB/s
 *   event queue 30.44  31.27  30.23  30.61   mean 30.64 GB/s
 *
 * The ranges overlap -- the event queue beat blocking on one run -- so the cost
 * is not measurable here, let alone the 3-12x the old sweep implied. Unfolded
 * (4800 requests instead of 120) it is the same story: 7.55 against 7.50.
 *
 * That is why this is now ON by default. A deadline that costs nothing and
 * turns 16 permanently lost threads into a bounded error is not a trade.
 *
 * The default is 60 s rather than something tight: a request measured 1.34 ms
 * here, so 60 s is four orders of magnitude of headroom and still bounds the
 * wedge. It is a backstop for an engine that has died, not a latency target.
 *
 * Depth 1 per thread is deliberate, and it is why this is a small change: the
 * thread pool, the concurrency and the RPC shape are all unchanged. The only
 * difference is that the call now has a deadline the caller owns.
 */
/*
 * A borrow-and-return pool of event queues.
 *
 * The obvious implementation -- one EQ per worker thread, thread_local -- was
 * written first and measured 35% SLOWER than blocking (2.08 vs 3.12 GB/s).
 * The reason is that the thread pool is deliberately oversized: 64 threads
 * serving at most `inflight` concurrent requests. Idle threads cost nothing,
 * but an idle EQ holds a network context, so that arrangement paid for 64
 * contexts to run 16 requests. Sizing the pool to the concurrency instead:
 *
 *   blocking, 64 threads    3.12 GB/s      eq, 64 EQs    2.08 GB/s
 *   blocking, 16 threads    3.12           eq, 16 EQs    3.24
 *                                          eq,  8 EQs    3.27
 *
 * Blocking is flat because unused threads are free; the event path is not,
 * which is most of what the original "EQs are expensive" finding was about.
 *
 * Borrowing fixes it without a tuning knob. An EQ is held only for the
 * duration of one request, so the pool grows to the actual high-water
 * concurrency and no further -- 16 here, whatever the thread count. It also
 * keeps the property the per-thread version had for free: no two threads ever
 * hold the same EQ, so a poll cannot harvest another thread's completion.
 */
class nixlDaosEqPool {
public:
    ~nixlDaosEqPool() {
        for (auto h : all_) {
            daos_eq_destroy(h, 0);
        }
    }

    bool
    borrow(daos_handle_t &out) {
        {
            std::lock_guard<std::mutex> g(m_);
            if (!free_.empty()) {
                out = free_.back();
                free_.pop_back();
                return true;
            }
        }
        /* Created outside the lock: daos_eq_create() talks to the client
         * library and is far too slow to hold a mutex across. */
        daos_handle_t h{};
        if (daos_eq_create(&h) != 0) {
            return false;
        }
        {
            std::lock_guard<std::mutex> g(m_);
            all_.push_back(h);
        }
        out = h;
        return true;
    }

    void
    giveBack(daos_handle_t h) {
        std::lock_guard<std::mutex> g(m_);
        free_.push_back(h);
    }

private:
    std::mutex m_;
    std::vector<daos_handle_t> free_; /* available now */
    std::vector<daos_handle_t> all_; /* every EQ ever made, for teardown */
};

/*
 * Submit. Each group is one daos_obj_fetch()/daos_obj_update().
 *
 * The work runs on a pool thread. Whether that thread then BLOCKS inside DAOS
 * or waits on an event queue with a deadline is decided by
 * NIXL_DAOS_EQ_TIMEOUT -- see the block above for what each costs and what is
 * still unmeasured. Either way prepXfer() has already folded the descriptor
 * list down to a handful of RPCs, so the thread count tracks requests rather
 * than descriptors.
 */
nixl_status_t
nixlDaosEngine::postXfer(const nixl_xfer_op_t &operation,
                         const nixl_meta_dlist_t &local,
                         const nixl_meta_dlist_t &remote,
                         const std::string &remote_agent,
                         nixlBackendReqH *&handle,
                         const nixl_opt_b_args_t *opt_args) const {
    auto *req = dynamic_cast<nixlDaosBackendReqH *>(handle);
    if (req == nullptr) {
        return NIXL_ERR_INVALID_PARAM;
    }
    if (req->groups.empty()) {
        return NIXL_SUCCESS;
    }

    req->status.store(NIXL_SUCCESS);
    req->pending.store(static_cast<unsigned>(req->groups.size()));
    req->posted = true;

    for (auto &g : req->groups) {
        nixlDaosIoGroup *gp = &g;
        pool_->submit([this, req, gp]() {
            daos_key_t dkey;
            d_iov_set(&dkey, &gp->dkeyVal, sizeof(uint64_t));

            const unsigned nr = static_cast<unsigned>(gp->iods.size());
            const double eq_timeout = eqTimeout_;
            daos_handle_t eq{};
            daos_event_t ev;
            daos_event_t *evp = nullptr;
            bool use_eq = false;
            int rc;

            if (eq_timeout > 0.0) {
                /* A queue we could not get is a reason to fall back to the
                 * blocking call, not to fail the transfer: the deadline is an
                 * improvement on the failure path, never a prerequisite for
                 * doing the I/O. */
                if (eqPool_->borrow(eq)) {
                    if (daos_event_init(&ev, eq, nullptr) == 0) {
                        use_eq = true;
                    } else {
                        eqPool_->giveBack(eq);
                    }
                }
                if (!use_eq) {
                    NIXL_ERROR << "DAOS: no event queue available, "
                                  "falling back to a blocking call";
                }
            }
            daos_event_t *ev_arg = use_eq ? &ev : nullptr;
#ifdef NIXL_DAOS_HAVE_GPU
            if (!gp->memAttrs.empty()) {
                /* The GPU entry points take an event exactly like the host
                 * ones (they differ only by the GPU_DIRECT flag and the
                 * memory attributes), so this path gets the same deadline. */
                rc = req->op == NIXL_READ ? daos_obj_fetch_gpu(gp->oh,
                                                               DAOS_TX_NONE,
                                                               0,
                                                               &dkey,
                                                               nr,
                                                               gp->iods.data(),
                                                               gp->sgls.data(),
                                                               gp->memAttrs.data(),
                                                               nullptr,
                                                               ev_arg) :
                                            daos_obj_update_gpu(gp->oh,
                                                                DAOS_TX_NONE,
                                                                0,
                                                                &dkey,
                                                                nr,
                                                                gp->iods.data(),
                                                                gp->sgls.data(),
                                                                gp->memAttrs.data(),
                                                                ev_arg);
            } else
#endif
            {
                rc = req->op == NIXL_READ ? daos_obj_fetch(gp->oh,
                                                           DAOS_TX_NONE,
                                                           0,
                                                           &dkey,
                                                           nr,
                                                           gp->iods.data(),
                                                           gp->sgls.data(),
                                                           nullptr,
                                                           ev_arg) :
                                            daos_obj_update(gp->oh,
                                                            DAOS_TX_NONE,
                                                            0,
                                                            &dkey,
                                                            nr,
                                                            gp->iods.data(),
                                                            gp->sgls.data(),
                                                            ev_arg);
            }

            if (use_eq) {
                if (rc == 0) {
                    /* Timeout is in microseconds. n == 0 means the deadline
                     * passed with the RPC still outstanding -- the thread is
                     * reclaimable, the operation is not, so abort it rather
                     * than leaving DAOS holding a pointer into a stack frame
                     * that is about to go away. Both calls measured at 0.00 s
                     * against a dead engine. */
                    const int n =
                        daos_eq_poll(eq, 1, static_cast<int64_t>(eq_timeout * 1e6), 1, &evp);
                    if (n == 0) {
                        NIXL_ERROR << "DAOS: no completion in " << eq_timeout
                                   << "s, abandoning the request";
                        daos_event_abort(&ev);
                        rc = -DER_TIMEDOUT;
                    } else {
                        rc = (n < 0) ? n : evp->ev_error;
                    }
                }
                daos_event_fini(&ev);
                eqPool_->giveBack(eq);
            }

            if (rc != 0) {
                NIXL_ERROR << (req->op == NIXL_READ ? "daos_obj_fetch" : "daos_obj_update")
                           << " failed: " << rc;
                req->recordError(daosToNixl(rc));
            } else if (req->op == NIXL_READ) {
                /* A fetch of a key that was never written returns success with
                 * iod_size 0. Silence there would be a miss reported as a hit,
                 * and this project has already paid for one of those. */
                for (size_t i = 0; i < gp->iods.size(); i++) {
                    if (gp->iods[i].iod_size == 0) {
                        NIXL_ERROR << "DAOS: read of an absent key (dkey " << gp->dkeyVal
                                   << ", akey " << gp->akeyVals[i] << ")";
                        req->recordError(NIXL_ERR_NOT_FOUND);
                        break;
                    }
                }
            }
            req->finishOne();
        });
    }

    return NIXL_IN_PROG;
}

nixl_status_t
nixlDaosEngine::checkXfer(nixlBackendReqH *handle) const {
    auto *req = dynamic_cast<nixlDaosBackendReqH *>(handle);
    if (req == nullptr) {
        return NIXL_ERR_INVALID_PARAM;
    }

    /* Prepared but not posted: nothing is running, so nothing can have
     * finished. Saying SUCCESS here would let a caller act on data that was
     * never fetched. */
    if (!req->posted) {
        return NIXL_IN_PROG;
    }
    if (req->pending.load() != 0) {
        return NIXL_IN_PROG;
    }

    return static_cast<nixl_status_t>(req->status.load());
}

nixl_status_t
nixlDaosEngine::releaseReqH(nixlBackendReqH *handle) const {
    auto *req = dynamic_cast<nixlDaosBackendReqH *>(handle);
    if (req == nullptr) {
        return NIXL_ERR_INVALID_PARAM;
    }

    /* DAOS has no cancel for a blocking call already in flight, so the only
     * safe way out is to let it finish: the group vectors the workers read
     * from die with this object. */
    if (req->posted) {
        req->waitDone();
    }

    delete req;
    return NIXL_SUCCESS;
}
