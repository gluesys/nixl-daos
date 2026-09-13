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

#ifndef __DAOS_BACKEND_H
#define __DAOS_BACKEND_H

#include <nixl.h>
#include <nixl_types.h>
#include "backend/backend_engine.h"
#include <daos.h>
#include <daos_obj.h>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * Design (ADR-nixl-001): no DFS. Each NIXL OBJ_SEG descriptor addresses one
 * DAOS object: oid derived from the key string, dkey = key, akey = the
 * descriptor's layer index (metaInfo) or "data". A NIXL transfer whose remote
 * side lists N descriptors with the same key becomes ONE daos_obj_fetch/update
 * carrying N iods, which is what removes the ~0.63 ms/object DFS fixed cost
 * measured in lmcache-daos (doc/LAYERWISE-MEASUREMENT.md).
 *
 * Descriptor contract (from NIXL BackendGuide, OBJ row):
 *   addr     = offset inside the value
 *   len      = 0 on registration, transfer length on prepXfer
 *   devId    = numeric object key (unused when metaInfo carries the string key)
 *   metaInfo = "<key>[/<akey>]"   -- key is the LMCache chunk hash
 * Backend init params:
 *   pool      : DAOS pool label or UUID            (required)
 *   container : DAOS container label or UUID       (required; NIXL "bucket")
 *   sys       : DAOS system name                   (default daos_server)
 *   oclass    : object class for new objects       (default RP_2GX)
 *   eq_depth  : max in-flight events per request   (default 64)
 */

enum nixl_daos_mem_type { NIXL_DAOS_MEM_DRAM = 0, NIXL_DAOS_MEM_OBJ = 1, NIXL_DAOS_MEM_VRAM = 2 };

class nixlDaosMetadata : public nixlBackendMD {
public:
    nixl_daos_mem_type type;
    explicit nixlDaosMetadata(nixl_daos_mem_type t) : nixlBackendMD(true), type(t) {}
};

class nixlDaosDramMetadata : public nixlDaosMetadata {
public:
    void *addr = nullptr;
    size_t len = 0;
    nixlDaosDramMetadata(void *a, size_t l) : nixlDaosMetadata(NIXL_DAOS_MEM_DRAM), addr(a), len(l) {}
};

class nixlDaosObjMetadata : public nixlDaosMetadata {
public:
    std::string key;    // dkey
    std::string akey;   // akey ("data" when metaInfo has no '/')
    daos_obj_id_t oid{};
    nixlDaosObjMetadata() : nixlDaosMetadata(NIXL_DAOS_MEM_OBJ) {}
};

// One in-flight DAOS call (fetch or update) carrying several iods/sgls.
struct nixlDaosOp {
    daos_handle_t oh{};
    daos_key_t dkey{};
    std::vector<daos_iod_t> iods;
    std::vector<d_sg_list_t> sgls;
    std::vector<d_iov_t> iovs;        // one per iod (single-segment sgls)
    std::vector<daos_recx_t> recxs;   // one per iod
    daos_event_t ev{};
    bool posted = false;
    bool done = false;
    int rc = 0;
};

class nixlDaosBackendReqH : public nixlBackendReqH {
public:
    nixl_xfer_op_t op;
    daos_handle_t eq{};
    std::list<nixlDaosOp *> ops;
    size_t completed = 0;
    nixl_status_t status = NIXL_IN_PROG;
    nixlDaosBackendReqH() = default;
};

class nixlDaosEngine : public nixlBackendEngine {
private:
    std::string pool_, cont_, sys_, oclass_;
    unsigned eq_depth_ = 64;
    daos_handle_t poh_{}, coh_{};
    bool connected_ = false;

    mutable std::mutex oh_lock_;
    mutable std::unordered_map<std::string, daos_handle_t> oh_cache_; // key -> open object handle

    nixl_status_t connectPool();
    nixl_status_t openObject(const nixlDaosObjMetadata &md, daos_handle_t &oh) const;
    static daos_obj_id_t oidFromKey(const std::string &key);
    void freeOp(nixlDaosOp *op) const;

public:
    explicit nixlDaosEngine(const nixlBackendInitParams *init_params);
    ~nixlDaosEngine();

    bool supportsNotif() const override { return false; }
    bool supportsRemote() const override { return false; }
    bool supportsLocal() const override { return true; }

    nixl_mem_list_t getSupportedMems() const override {
        nixl_mem_list_t mems = {OBJ_SEG, DRAM_SEG};
#ifdef NIXL_DAOS_GPU
        mems.push_back(VRAM_SEG);
#endif
        return mems;
    }

    nixl_status_t connect(const std::string &) override { return NIXL_SUCCESS; }
    nixl_status_t disconnect(const std::string &) override { return NIXL_SUCCESS; }
    nixl_status_t loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) override { output = input; return NIXL_SUCCESS; }
    nixl_status_t unloadMD(nixlBackendMD *) override { return NIXL_SUCCESS; }

    nixl_status_t registerMem(const nixlBlobDesc &mem, const nixl_mem_t &nixl_mem, nixlBackendMD *&out) override;
    nixl_status_t deregisterMem(nixlBackendMD *meta) override;

    nixl_status_t prepXfer(const nixl_xfer_op_t &operation, const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote, const std::string &remote_agent,
                           nixlBackendReqH *&handle, const nixl_opt_b_args_t *opt_args = nullptr) const override;
    nixl_status_t postXfer(const nixl_xfer_op_t &operation, const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote, const std::string &remote_agent,
                           nixlBackendReqH *&handle, const nixl_opt_b_args_t *opt_args = nullptr) const override;
    nixl_status_t checkXfer(nixlBackendReqH *handle) const override;
    nixl_status_t releaseReqH(nixlBackendReqH *handle) const override;
};

#endif
