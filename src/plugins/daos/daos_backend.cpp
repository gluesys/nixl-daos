/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Gluesys Co., Ltd.
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
#include <cstring>
#include <functional>
#include <iostream>

// ---------------------------------------------------------------------------
// Phase 0 skeleton. The control flow and data structures are final; the DAOS
// calls are written against the 2.8 client API and must be compiled inside the
// daos-client image (daos-devel). Nothing here has run yet.
// ---------------------------------------------------------------------------

static const char *kDefaultAkey = "data";

static std::string
param(const nixlBackendInitParams *p, const char *k, const std::string &def) {
    if (p && p->customParams) {
        auto it = p->customParams->find(k);
        if (it != p->customParams->end()) return it->second;
    }
    return def;
}

nixlDaosEngine::nixlDaosEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params) {
    pool_   = param(init_params, "pool", "");
    cont_   = param(init_params, "container", "");
    sys_    = param(init_params, "sys", "daos_server");
    oclass_ = param(init_params, "oclass", "RP_2GX");
    eq_depth_ = std::stoul(param(init_params, "eq_depth", "64"));
    if (pool_.empty() || cont_.empty()) {
        std::cerr << "[DAOS] init params 'pool' and 'container' are required\n";
        this->initErr = true;
        return;
    }
    if (connectPool() != NIXL_SUCCESS) this->initErr = true;
}

nixlDaosEngine::~nixlDaosEngine() {
    for (auto &kv : oh_cache_) daos_obj_close(kv.second, nullptr);
    if (connected_) {
        daos_cont_close(coh_, nullptr);
        daos_pool_disconnect(poh_, nullptr);
        daos_fini();
    }
}

nixl_status_t
nixlDaosEngine::connectPool() {
    int rc = daos_init();
    if (rc) { std::cerr << "[DAOS] daos_init rc=" << rc << "\n"; return NIXL_ERR_BACKEND; }
    rc = daos_pool_connect(pool_.c_str(), sys_.c_str(), DAOS_PC_RW, &poh_, nullptr, nullptr);
    if (rc) { std::cerr << "[DAOS] pool connect(" << pool_ << ") rc=" << rc << "\n"; daos_fini(); return NIXL_ERR_BACKEND; }
    rc = daos_cont_open(poh_, cont_.c_str(), DAOS_COO_RW, &coh_, nullptr, nullptr);
    if (rc) { std::cerr << "[DAOS] cont open(" << cont_ << ") rc=" << rc << "\n"; daos_pool_disconnect(poh_, nullptr); daos_fini(); return NIXL_ERR_BACKEND; }
    connected_ = true;
    return NIXL_SUCCESS;
}

// Deterministic oid from the key so that every client derives the same object
// for the same LMCache chunk without a lookup. The low 64 bits come from a
// stable hash; daos_obj_generate_oid fills the class/type bits.
daos_obj_id_t
nixlDaosEngine::oidFromKey(const std::string &key) {
    daos_obj_id_t oid{};
    oid.lo = std::hash<std::string>{}(key);
    oid.hi = 0;
    return oid;
}

nixl_status_t
nixlDaosEngine::openObject(const nixlDaosObjMetadata &md, daos_handle_t &oh) const {
    std::lock_guard<std::mutex> g(oh_lock_);
    auto it = oh_cache_.find(md.key);
    if (it != oh_cache_.end()) { oh = it->second; return NIXL_SUCCESS; }
    daos_obj_id_t oid = md.oid;
    // TODO(phase1): map oclass_ string -> daos_oclass_id_t via daos_oclass_name2id
    int rc = daos_obj_generate_oid(coh_, &oid, DAOS_OT_MULTI_HASHED, OC_UNKNOWN, 0, 0);
    if (rc) return NIXL_ERR_BACKEND;
    rc = daos_obj_open(coh_, oid, DAOS_OO_RW, &oh, nullptr);
    if (rc) return NIXL_ERR_BACKEND;
    oh_cache_[md.key] = oh;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlDaosEngine::registerMem(const nixlBlobDesc &mem, const nixl_mem_t &nixl_mem, nixlBackendMD *&out) {
    switch (nixl_mem) {
    case DRAM_SEG:
        out = new nixlDaosDramMetadata(reinterpret_cast<void *>(mem.addr), mem.len);
        return NIXL_SUCCESS;
    case OBJ_SEG: {
        auto *md = new nixlDaosObjMetadata();
        // metaInfo = "<key>[/<akey>]"; fall back to devId when metaInfo is empty.
        std::string mi = mem.metaInfo;
        if (mi.empty()) mi = std::to_string(mem.devId);
        auto slash = mi.find('/');
        md->key  = (slash == std::string::npos) ? mi : mi.substr(0, slash);
        md->akey = (slash == std::string::npos) ? kDefaultAkey : mi.substr(slash + 1);
        md->oid  = oidFromKey(md->key);
        out = md;
        return NIXL_SUCCESS;
    }
#ifdef NIXL_DAOS_GPU
    case VRAM_SEG:
        // GPU-direct path: requires daos_mem_attr_t from the unmerged draft.
        // Keep the CUDA context rule from lmcache-daos (cuCtxSetCurrent per I/O thread).
        return NIXL_ERR_NOT_SUPPORTED;
#endif
    default:
        return NIXL_ERR_NOT_SUPPORTED;
    }
}

nixl_status_t
nixlDaosEngine::deregisterMem(nixlBackendMD *meta) {
    delete meta;
    return NIXL_SUCCESS;
}

// Build one nixlDaosOp per distinct (key) on the OBJ side; each descriptor
// pair becomes one iod (akey) + one single-iov sgl pointing at the DRAM side.
nixl_status_t
nixlDaosEngine::prepXfer(const nixl_xfer_op_t &operation, const nixl_meta_dlist_t &local,
                         const nixl_meta_dlist_t &remote, const std::string &, nixlBackendReqH *&handle,
                         const nixl_opt_b_args_t *) const {
    if (local.descCount() != remote.descCount() || local.descCount() == 0) return NIXL_ERR_INVALID_PARAM;
    if (local.getType() != DRAM_SEG || remote.getType() != OBJ_SEG) return NIXL_ERR_NOT_SUPPORTED;

    auto *req = new nixlDaosBackendReqH();
    req->op = operation;
    if (daos_eq_create(&req->eq)) { delete req; return NIXL_ERR_BACKEND; }

    std::unordered_map<std::string, nixlDaosOp *> by_key;
    for (int i = 0; i < local.descCount(); ++i) {
        auto *dmd = static_cast<nixlDaosDramMetadata *>(local[i].metadataP);
        auto *omd = static_cast<nixlDaosObjMetadata *>(remote[i].metadataP);
        (void)dmd;
        nixlDaosOp *op;
        auto it = by_key.find(omd->key);
        if (it == by_key.end()) {
            op = new nixlDaosOp();
            if (openObject(*omd, op->oh) != NIXL_SUCCESS) { delete op; releaseReqH(req); return NIXL_ERR_BACKEND; }
            d_iov_set(&op->dkey, const_cast<char *>(omd->key.data()), omd->key.size());
            by_key[omd->key] = op;
            req->ops.push_back(op);
        } else {
            op = it->second;
        }
        daos_iod_t iod{};
        d_iov_set(&iod.iod_name, const_cast<char *>(omd->akey.data()), omd->akey.size());
        iod.iod_type = DAOS_IOD_ARRAY;
        iod.iod_size = 1;
        iod.iod_nr = 1;
        daos_recx_t recx{};
        recx.rx_idx = remote[i].addr;   // offset inside the value
        recx.rx_nr  = local[i].len;
        op->recxs.push_back(recx);
        op->iods.push_back(iod);
        d_iov_t iov{};
        d_iov_set(&iov, reinterpret_cast<void *>(local[i].addr), local[i].len);
        op->iovs.push_back(iov);
        d_sg_list_t sgl{};
        sgl.sg_nr = 1;
        op->sgls.push_back(sgl);
    }
    // Fix up pointers after the vectors stopped growing.
    for (auto *op : req->ops) {
        for (size_t k = 0; k < op->iods.size(); ++k) {
            op->iods[k].iod_recxs = &op->recxs[k];
            op->sgls[k].sg_iovs = &op->iovs[k];
        }
    }
    handle = req;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlDaosEngine::postXfer(const nixl_xfer_op_t &operation, const nixl_meta_dlist_t &, const nixl_meta_dlist_t &,
                         const std::string &, nixlBackendReqH *&handle, const nixl_opt_b_args_t *) const {
    auto *req = static_cast<nixlDaosBackendReqH *>(handle);
    for (auto *op : req->ops) {
        if (op->posted) continue;
        if (daos_event_init(&op->ev, req->eq, nullptr)) return NIXL_ERR_BACKEND;
        int rc = (operation == NIXL_READ)
            ? daos_obj_fetch(op->oh, DAOS_TX_NONE, 0, &op->dkey, op->iods.size(), op->iods.data(), op->sgls.data(), nullptr, &op->ev)
            : daos_obj_update(op->oh, DAOS_TX_NONE, 0, &op->dkey, op->iods.size(), op->iods.data(), op->sgls.data(), &op->ev);
        if (rc) { op->rc = rc; op->done = true; req->status = NIXL_ERR_BACKEND; continue; }
        op->posted = true;
    }
    return checkXfer(handle);
}

nixl_status_t
nixlDaosEngine::checkXfer(nixlBackendReqH *handle) const {
    auto *req = static_cast<nixlDaosBackendReqH *>(handle);
    if (req->status != NIXL_IN_PROG) return req->status;
    daos_event_t *evs[64];
    int n = daos_eq_poll(req->eq, 1, DAOS_EQ_NOWAIT, 64, evs);
    if (n < 0) { req->status = NIXL_ERR_BACKEND; return req->status; }
    for (int i = 0; i < n; ++i) {
        if (evs[i]->ev_error) req->status = NIXL_ERR_BACKEND;
        req->completed++;
    }
    if (req->status == NIXL_IN_PROG && req->completed == req->ops.size()) req->status = NIXL_SUCCESS;
    return req->status;
}

void
nixlDaosEngine::freeOp(nixlDaosOp *op) const {
    if (op->posted) daos_event_fini(&op->ev);
    delete op;
}

nixl_status_t
nixlDaosEngine::releaseReqH(nixlBackendReqH *handle) const {
    auto *req = static_cast<nixlDaosBackendReqH *>(handle);
    for (auto *op : req->ops) freeOp(op);
    if (req->eq.cookie) daos_eq_destroy(req->eq, 0);
    delete req;
    return NIXL_SUCCESS;
}
